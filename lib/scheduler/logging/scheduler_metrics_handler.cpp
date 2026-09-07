// SPDX-FileCopyrightText: Copyright (C) 2021-2026 Software Radio Systems Limited
// SPDX-License-Identifier: BSD-3-Clause-Open-MPI
// Portions of this file may implement 3GPP specifications, which may be subject to additional licensing requirements.

#include "scheduler_metrics_handler.h"
#include "../config/cell_configuration.h"
#include "../uci_scheduling/uci_indication_selector.h"
#include "bsr_ml_dataset_logger.h"
#include "csi_ml_dataset_logger.h"
#include "slice_ml_dataset_logger.h"
#include "ocudu/ocudulog/ocudulog.h"
#include "ocudu/ran/resource_allocation/rb_bitmap.h"
#include "ocudu/ran/slot_point.h"
#include "ocudu/scheduler/result/sched_result.h"
#include "ocudu/scheduler/scheduler_rach_handler.h"
#include <cmath>
#include <map>
#include <utility>

using namespace ocudu;

namespace {

class null_metrics_notifier final : public scheduler_cell_metrics_notifier
{
private:
  scheduler_cell_metrics& get_next() override { return null_report; }

  void commit(scheduler_cell_metrics& report) override
  {
    // do nothing
    null_report.ue_metrics.clear();
    null_report.events.clear();
  }
  bool is_sched_report_required(slot_point_extended sl_tx) const override { return false; }

  scheduler_cell_metrics null_report{};
};

} // namespace

static null_metrics_notifier null_notifier;

cell_metrics_handler::cell_metrics_handler(
    const cell_configuration&                                                      cell_cfg_,
    const std::optional<sched_cell_configuration_request_message::metrics_config>& metrics_cfg) :
  notifier(metrics_cfg.has_value() and metrics_cfg->notifier != nullptr ? *metrics_cfg->notifier : null_notifier),
  cell_cfg(cell_cfg_),
  nof_slots_per_sf(get_nof_slots_per_subframe(cell_cfg.scs_common))
{
  if (not enabled()) {
    return;
  }

  // Pre-reserve space.
  ues.reserve(MAX_NOF_DU_UES);
  rnti_to_ue_index_lookup.reserve(MAX_NOF_DU_UES);
  const unsigned pre_reserved_event_capacity = std::min(3U * MAX_NOF_DU_UES, metrics_cfg->max_ue_events_per_report);
  pending_events.reserve(pre_reserved_event_capacity);
  unsigned tdd_period_slots =
      cell_cfg.tdd_cfg_common.has_value() ? nof_slots_per_tdd_period(*cell_cfg.tdd_cfg_common) : 0U;
  ul_prbs_used_per_tdd_slot_idx.resize(tdd_period_slots);
  dl_prbs_used_per_tdd_slot_idx.resize(tdd_period_slots);
}

cell_metrics_handler::~cell_metrics_handler() {}

bool cell_metrics_handler::enabled() const
{
  return &notifier != &null_notifier;
}

void cell_metrics_handler::handle_ue_creation(du_ue_index_t ue_index, rnti_t rnti, pci_t pcell_pci)
{
  if (not enabled()) {
    return;
  }

  ues.emplace(ue_index);
  ues[ue_index].rnti     = rnti;
  ues[ue_index].ue_index = ue_index;
  ues[ue_index].pci      = pcell_pci;
  rnti_to_ue_index_lookup.emplace(rnti, ue_index);

  if (pending_events.size() < pending_events.capacity()) {
    pending_events.push_back(
        scheduler_cell_event{last_slot_tx.without_hyper_sfn(), rnti, scheduler_cell_event::event_type::ue_add});
  } else {
    data.filtered_events_counter++;
  }
}

void cell_metrics_handler::handle_ue_reconfiguration(du_ue_index_t ue_index)
{
  if (not enabled()) {
    return;
  }
  if (pending_events.size() < pending_events.capacity()) {
    pending_events.push_back(scheduler_cell_event{
        last_slot_tx.without_hyper_sfn(), ues[ue_index].rnti, scheduler_cell_event::event_type::ue_reconf});
  } else {
    data.filtered_events_counter++;
  }
}

void cell_metrics_handler::handle_ue_slice_update(du_ue_index_t ue_index, const s_nssai_t& s_nssai)
{
  if (not enabled()) {
    return;
  }
  if (ues.contains(ue_index)) {
    ues[ue_index].s_nssai = s_nssai;
  }
}

void cell_metrics_handler::handle_ue_deletion(du_ue_index_t ue_index)
{
  if (not enabled()) {
    return;
  }
  if (ues.contains(ue_index)) {
    rnti_t rnti = ues[ue_index].rnti;

    if (pending_events.size() < pending_events.capacity()) {
      pending_events.push_back(
          scheduler_cell_event{last_slot_tx.without_hyper_sfn(), rnti, scheduler_cell_event::event_type::ue_rem});
    } else {
      data.filtered_events_counter++;
    }

    rnti_to_ue_index_lookup.erase(rnti);
    ues.erase(ue_index);
  }
}

void cell_metrics_handler::handle_rach_indication(const rach_indication_message& msg, slot_point sl_tx)
{
  if (not enabled()) {
    return;
  }
  unsigned slot_diff = sl_tx - msg.slot_rx;
  for (const auto& occ : msg.occasions) {
    data.nof_prach_preambles += occ.preambles.size();
    data.sum_prach_delay_slots += slot_diff * occ.preambles.size();
  }
}

void cell_metrics_handler::handle_msg3_crc_indication(const ul_crc_pdu_indication& crc_pdu)
{
  if (not enabled()) {
    return;
  }

  if (crc_pdu.tb_crc_success) {
    data.nof_msg3_ok++;
  } else {
    data.nof_msg3_nok++;
  }
}

void cell_metrics_handler::handle_crc_indication(slot_point                   sl_rx,
                                                 const ul_crc_pdu_indication& crc_pdu,
                                                 units::bytes                 tbs)
{
  if (not enabled()) {
    return;
  }
  if (ues.contains(crc_pdu.ue_index)) {
    auto& u = ues[crc_pdu.ue_index];
    u.data.count_crc_acks += crc_pdu.tb_crc_success ? 1 : 0;
    ++u.data.count_crc_pdus;
    if (crc_pdu.ul_sinr_dB.has_value()) {
      ++u.data.nof_pusch_snr_reports;
      u.data.sum_pusch_snrs += crc_pdu.ul_sinr_dB.value();
    }
    if (crc_pdu.ul_rsrp_dBFS.has_value()) {
      ++u.data.nof_pusch_rsrp_reports;
      u.data.sum_pusch_rsrp += crc_pdu.ul_rsrp_dBFS.value();
    }
    if (crc_pdu.tb_crc_success) {
      u.data.sum_ul_tb_bytes += tbs.value();
    }
    if (crc_pdu.time_advance_offset.has_value()) {
      u.data.ta.update(crc_pdu.time_advance_offset.value().to_seconds());
      u.data.pusch_ta.update(crc_pdu.time_advance_offset.value().to_seconds());
    }
    u.data.sum_crc_delay_slots += last_slot_tx.without_hyper_sfn() - sl_rx;
    u.data.max_crc_delay_slots =
        std::max(static_cast<unsigned>(last_slot_tx.without_hyper_sfn() - sl_rx), u.data.max_crc_delay_slots);
  }
}

void cell_metrics_handler::handle_srs_indication(const srs_indication::srs_indication_pdu& srs_pdu, unsigned ri)
{
  if (not enabled()) {
    return;
  }
  if (ues.contains(srs_pdu.ue_index)) {
    auto& u = ues[srs_pdu.ue_index];
    if (srs_pdu.time_advance_offset.has_value()) {
      u.data.ta.update(srs_pdu.time_advance_offset.value().to_seconds());
      u.data.srs_ta.update(srs_pdu.time_advance_offset.value().to_seconds());
      u.data.ul_ri.update(ri);
    }
  }
}

void cell_metrics_handler::handle_pucch_sinr(ue_metric_context& u, float sinr)
{
  ++u.data.nof_pucch_snr_reports;
  u.data.sum_pucch_snrs += sinr;
}

void cell_metrics_handler::handle_csi_report(ue_metric_context& u, const csi_report_data& csi)
{
  // Add new CQI and RI observations if they are available in the CSI report.
  if (csi.first_tb_wideband_cqi.has_value()) {
    u.data.cqi.update(csi.first_tb_wideband_cqi->value());
  }
  if (csi.ri.has_value()) {
    u.data.dl_ri.update(csi.ri->value());
  }
}

void cell_metrics_handler::handle_uci_with_harq_ack(du_ue_index_t ue_index, slot_point sl_rx, bool pucch)
{
  if (ues.contains(ue_index)) {
    auto& u    = ues[ue_index];
    auto  diff = last_slot_tx.without_hyper_sfn() - sl_rx;
    if (pucch) {
      u.data.sum_pucch_harq_delay_slots += diff;
      u.data.max_pucch_harq_delay_slots = std::max(static_cast<unsigned>(diff), u.data.max_pucch_harq_delay_slots);
      ++u.data.count_pucch_harq_pdus;
    } else {
      u.data.sum_pusch_harq_delay_slots += diff;
      u.data.max_pusch_harq_delay_slots = std::max(static_cast<unsigned>(diff), u.data.max_pusch_harq_delay_slots);
      ++u.data.count_pusch_harq_pdus;
    }
  }
}

void cell_metrics_handler::handle_dl_harq_ack(du_ue_index_t ue_index, bool ack, units::bytes tbs)
{
  if (ues.contains(ue_index)) {
    auto& u = ues[ue_index];
    u.data.count_uci_harq_acks += ack ? 1 : 0;
    ++u.data.count_uci_harqs;
    if (ack) {
      u.data.sum_dl_tb_bytes += tbs.value();
    }
  }
}

void cell_metrics_handler::handle_harq_timeout(du_ue_index_t ue_index, bool is_dl)
{
  if (ues.contains(ue_index)) {
    auto& u = ues[ue_index];
    if (is_dl) {
      ++u.data.count_uci_harqs;
    } else {
      ++u.data.count_crc_pdus;
    }
  }
}

void cell_metrics_handler::handle_uci_pdu_indication(du_ue_index_t ue_index, const uci_action& action)
{
  if (not enabled()) {
    return;
  }
  if (ues.contains(ue_index)) {
    auto& u = ues[ue_index];

    if (action.ul_sinr_dB.has_value()) {
      handle_pucch_sinr(u, *action.ul_sinr_dB);
    }

    if (action.time_advance_offset.has_value()) {
      u.data.ta.update(action.time_advance_offset->to_seconds());
      u.data.pucch_ta.update(action.time_advance_offset->to_seconds());
    }

    if (not action.uci_valid and not action.harq_ack_bits.empty()) {
      switch (action.type) {
        case uci_action::pdu_type::pucch_f0f1:
          ++u.data.nof_pucch_f0f1_invalid_harqs;
          break;
        case uci_action::pdu_type::pucch_f2f3f4:
          ++u.data.nof_pucch_f2f3f4_invalid_harqs;
          break;
        default:
          ++u.data.nof_pusch_invalid_harqs;
      }
    }

    if (action.csi.has_value()) {
      if (action.csi->valid) {
        handle_csi_report(u, action.csi.value());
      } else {
        if (action.type == uci_action::pdu_type::pucch_f2f3f4) {
          ++u.data.nof_pucch_f2f3f4_invalid_csis;
        } else {
          ++u.data.nof_pusch_invalid_csis;
        }
      }
    }
  }
}

void cell_metrics_handler::handle_sr_indication(du_ue_index_t ue_index, slot_point sr_slot)
{
  if (ues.contains(ue_index)) {
    auto& u = ues[ue_index];
    if (not u.data.last_sr_slot.valid()) {
      u.data.last_sr_slot = sr_slot;
    }
    ++u.data.count_sr;
  }
}

void cell_metrics_handler::handle_ul_bsr_indication(const ul_bsr_indication_message& bsr)
{
  if (not enabled()) {
    return;
  }
  if (ues.contains(bsr.ue_index)) {
    auto& u = ues[bsr.ue_index];

    // Store last BSR.
    u.last_bsr = 0;
    // TODO: Handle different BSR formats.
    for (unsigned i = 0; i != bsr.reported_lcgs.size(); ++i) {
      u.last_bsr += bsr.reported_lcgs[i].nof_bytes;
    }
  }
}

void cell_metrics_handler::handle_ul_phr_indication(const ul_phr_indication_message& phr_ind)
{
  if (not enabled()) {
    return;
  }
  if (ues.contains(phr_ind.ue_index)) {
    auto& u = ues[phr_ind.ue_index];

    // Store last PHR.
    if (not phr_ind.phr.get_phr().empty()) {
      // Log the floor of the average of the PH interval.
      interval<int> rg = phr_ind.phr.get_phr().front().ph;
      u.last_phr       = (rg.start() + rg.stop()) / 2;
      auto diff        = last_slot_tx.without_hyper_sfn() - phr_ind.slot_rx;
      u.data.sum_ul_ce_delay_slots += diff;
      u.data.max_ul_ce_delay_slots = std::max(static_cast<unsigned>(diff), u.data.max_ul_ce_delay_slots);
      ++u.data.nof_ul_ces;
    }
  }
}

void cell_metrics_handler::handle_dl_buffer_state_indication(const dl_buffer_state_indication_message& dl_bs)
{
  if (not enabled()) {
    return;
  }
  if (ues.contains(dl_bs.ue_index)) {
    auto& u = ues[dl_bs.ue_index];

    // Store last DL buffer state.
    u.last_dl_bs[dl_bs.lcid] = dl_bs.bs;
  }
}

void cell_metrics_handler::handle_error_indication()
{
  ++data.error_indication_counter;
}

void cell_metrics_handler::handle_late_dl_harqs()
{
  ++data.nof_failed_pdsch_allocs_late_harqs;
}

void cell_metrics_handler::handle_late_ul_harqs()
{
  ++data.nof_failed_pusch_allocs_late_harqs;
}

void cell_metrics_handler::report_metrics()
{
  auto next_report = notifier.get_builder();

  const std::chrono::milliseconds report_period{data.nof_slots / last_slot_tx.nof_slots_per_subframe()};

  const bool     slice_ml_on      = slice_ml_dataset::is_ue_enabled() or slice_ml_dataset::is_slicemanager_enabled();
  const uint64_t slice_ml_period  = slice_ml_on ? slice_ml_dataset::next_period_index() : 0;
  const uint32_t slice_ml_slot    = last_slot_tx.without_hyper_sfn().system_slot();
  const auto     slice_ml_period_ms = static_cast<uint32_t>(report_period.count());

  struct slice_accum {
    uint32_t nof_ues           = 0;
    double   dl_brate_sum      = 0.0;
    double   ul_brate_sum      = 0.0;
    uint64_t dl_bs_sum         = 0;
    uint64_t bsr_sum           = 0;
    uint64_t dl_ok             = 0;
    uint64_t dl_nok            = 0;
    uint64_t ul_ok             = 0;
    uint64_t ul_nok            = 0;
    double   cqi_sum           = 0.0;
    unsigned cqi_n             = 0;
    double   dl_mcs_sum        = 0.0;
    double   ul_mcs_sum        = 0.0;
    unsigned mcs_n             = 0;
    double   snr_sum           = 0.0;
    unsigned snr_n             = 0;
    double   sr_delay_sum      = 0.0;
    unsigned sr_delay_n        = 0;
    float    sr_delay_max      = 0.0f;
    bool     has_sr_delay_max  = false;
    unsigned embb_ok           = 0;
    unsigned urllc_ok          = 0;
  };
  std::map<std::pair<uint8_t, uint32_t>, slice_accum> slice_accums;

  const float slice_ml_target_rate = slice_ml_dataset::configured_target_dl_rate_kbps();
  const float slice_ml_budget_ms   = slice_ml_dataset::configured_delay_budget_ms();

  for (ue_metric_context& ue : ues) {
    const unsigned nof_ul_grants  = ue.data.nof_puschs;
    const uint64_t ul_tb_bytes    = ue.data.sum_ul_tb_bytes;
    // Compute statistics of the UE metrics and push the result to the report.
    scheduler_ue_metrics ue_report = ue.compute_report(report_period, nof_slots_per_sf);
    ue.last_dl_brate_kbps          = ue_report.dl_brate_kbps;
    ue.last_ul_brate_kbps          = ue_report.ul_brate_kbps;
    ue.last_sr_count               = ue_report.sr_count;
    ue.last_nof_ul_grants          = nof_ul_grants;
    ue.last_ul_tb_bytes            = ul_tb_bytes;
    next_report->ue_metrics.push_back(ue_report);

    // Measurement-only: mirror this UE's periodic DL link-adaptation KPIs (throughput / BLER / MCS)
    // into the CSI-ML KPI dataset when enabled, so a ZOH-vs-ML A/B run can be compared on delivered
    // performance rather than prediction error alone. Reads the report just built; changes nothing.
    if (csi_ml_dataset::is_kpi_enabled()) {
      csi_ml_dataset::kpi_sample ks{};
      ks.slot          = last_slot_tx.without_hyper_sfn().system_slot();
      ks.rnti          = static_cast<uint16_t>(ue_report.rnti);
      ks.ue_index      = static_cast<uint16_t>(ue_report.ue_index);
      ks.dl_brate_kbps = ue_report.dl_brate_kbps;
      ks.dl_nof_ok     = ue_report.dl_nof_ok;
      ks.dl_nof_nok    = ue_report.dl_nof_nok;
      ks.dl_mcs        = static_cast<uint8_t>(ue_report.dl_mcs.value());
      ks.mean_cqi      = ue_report.cqi_stats.get_nof_observations() > 0
                             ? static_cast<float>(ue_report.cqi_stats.get_mean())
                             : std::numeric_limits<float>::quiet_NaN();
      ks.dl_ri = ue_report.dl_ri_stats.get_nof_observations() > 0
                     ? static_cast<float>(ue_report.dl_ri_stats.get_mean())
                     : std::numeric_limits<float>::quiet_NaN();
      csi_ml_dataset::log_kpi_sample(ks);
    }

    if (slice_ml_on) {
      const uint8_t  sst = ue_report.s_nssai.sst.value();
      const uint32_t sd  = ue_report.s_nssai.sd.value();

      const bool  has_cqi  = ue_report.cqi_stats.get_nof_observations() > 0;
      const float mean_cqi = has_cqi ? ue_report.cqi_stats.get_mean() : 0.0f;
      const bool  has_dlri = ue_report.dl_ri_stats.get_nof_observations() > 0;
      const bool  has_ulri = ue_report.ul_ri_stats.get_nof_observations() > 0;

      if (slice_ml_dataset::is_ue_enabled()) {
        slice_ml_dataset::ue_sample us{};
        us.period_idx = slice_ml_period;
        us.slot       = slice_ml_slot;
        us.period_ms  = slice_ml_period_ms;
        us.rnti       = static_cast<uint16_t>(ue_report.rnti);
        us.ue_index   = static_cast<uint16_t>(ue_report.ue_index);
        us.sst        = sst;
        us.sd         = sd;

        us.dl_brate_kbps = ue_report.dl_brate_kbps;
        us.ul_brate_kbps = ue_report.ul_brate_kbps;
        us.dl_nof_ok     = ue_report.dl_nof_ok;
        us.dl_nof_nok    = ue_report.dl_nof_nok;
        us.ul_nof_ok     = ue_report.ul_nof_ok;
        us.ul_nof_nok    = ue_report.ul_nof_nok;

        us.tot_pdsch_prbs_used = ue_report.tot_pdsch_prbs_used;
        us.tot_pusch_prbs_used = ue_report.tot_pusch_prbs_used;

        us.dl_mcs       = static_cast<uint8_t>(ue_report.dl_mcs.value());
        us.ul_mcs       = static_cast<uint8_t>(ue_report.ul_mcs.value());
        us.has_mean_cqi = has_cqi;
        us.mean_cqi     = mean_cqi;
        us.has_dl_ri    = has_dlri;
        us.dl_ri        = has_dlri ? ue_report.dl_ri_stats.get_mean() : 0.0f;
        us.has_ul_ri    = has_ulri;
        us.ul_ri        = has_ulri ? ue_report.ul_ri_stats.get_mean() : 0.0f;
        us.pusch_snr_db = ue_report.pusch_snr_db;
        us.pusch_rsrp_db = ue_report.pusch_rsrp_db;
        us.pucch_snr_db  = ue_report.pucch_snr_db;

        us.bsr      = ue_report.bsr;
        us.dl_bs    = ue_report.dl_bs;
        us.sr_count = ue_report.sr_count;

        us.has_avg_sr_to_pusch_delay_ms = ue_report.avg_sr_to_pusch_delay_ms.has_value();
        us.avg_sr_to_pusch_delay_ms     = us.has_avg_sr_to_pusch_delay_ms ? *ue_report.avg_sr_to_pusch_delay_ms : 0.0f;
        us.has_max_sr_to_pusch_delay_ms = ue_report.max_sr_to_pusch_delay_ms.has_value();
        us.max_sr_to_pusch_delay_ms     = us.has_max_sr_to_pusch_delay_ms ? *ue_report.max_sr_to_pusch_delay_ms : 0.0f;
        us.has_avg_crc_delay_ms         = ue_report.avg_crc_delay_ms.has_value();
        us.avg_crc_delay_ms             = us.has_avg_crc_delay_ms ? *ue_report.avg_crc_delay_ms : 0.0f;
        us.has_max_crc_delay_ms         = ue_report.max_crc_delay_ms.has_value();
        us.max_crc_delay_ms             = us.has_max_crc_delay_ms ? *ue_report.max_crc_delay_ms : 0.0f;
        us.has_avg_pusch_harq_delay_ms  = ue_report.avg_pusch_harq_delay_ms.has_value();
        us.avg_pusch_harq_delay_ms      = us.has_avg_pusch_harq_delay_ms ? *ue_report.avg_pusch_harq_delay_ms : 0.0f;
        us.has_max_pusch_harq_delay_ms  = ue_report.max_pusch_harq_delay_ms.has_value();
        us.max_pusch_harq_delay_ms      = us.has_max_pusch_harq_delay_ms ? *ue_report.max_pusch_harq_delay_ms : 0.0f;
        us.has_avg_pucch_harq_delay_ms  = ue_report.avg_pucch_harq_delay_ms.has_value();
        us.avg_pucch_harq_delay_ms      = us.has_avg_pucch_harq_delay_ms ? *ue_report.avg_pucch_harq_delay_ms : 0.0f;
        us.has_max_pucch_harq_delay_ms  = ue_report.max_pucch_harq_delay_ms.has_value();
        us.max_pucch_harq_delay_ms      = us.has_max_pucch_harq_delay_ms ? *ue_report.max_pucch_harq_delay_ms : 0.0f;
        us.has_avg_ce_delay_ms          = ue_report.avg_ce_delay_ms.has_value();
        us.avg_ce_delay_ms              = us.has_avg_ce_delay_ms ? *ue_report.avg_ce_delay_ms : 0.0f;
        us.has_max_ce_delay_ms          = ue_report.max_ce_delay_ms.has_value();
        us.max_ce_delay_ms              = us.has_max_ce_delay_ms ? *ue_report.max_ce_delay_ms : 0.0f;

        us.has_last_phr     = ue_report.last_phr.has_value();
        us.last_phr         = us.has_last_phr ? *ue_report.last_phr : 0;
        us.has_last_dl_olla = ue_report.last_dl_olla.has_value();
        us.last_dl_olla     = us.has_last_dl_olla ? *ue_report.last_dl_olla : 0.0f;
        us.has_last_ul_olla = ue_report.last_ul_olla.has_value();
        us.last_ul_olla     = us.has_last_ul_olla ? *ue_report.last_ul_olla : 0.0f;

        slice_ml_dataset::log_ue_sample(us);
      }

      if (slice_ml_dataset::is_slicemanager_enabled()) {
        slice_accum& acc = slice_accums[{sst, sd}];
        ++acc.nof_ues;
        acc.dl_brate_sum += ue_report.dl_brate_kbps;
        acc.ul_brate_sum += ue_report.ul_brate_kbps;
        acc.dl_bs_sum += ue_report.dl_bs;
        acc.bsr_sum += ue_report.bsr;
        acc.dl_ok += ue_report.dl_nof_ok;
        acc.dl_nok += ue_report.dl_nof_nok;
        acc.ul_ok += ue_report.ul_nof_ok;
        acc.ul_nok += ue_report.ul_nof_nok;
        if (has_cqi) {
          acc.cqi_sum += mean_cqi;
          ++acc.cqi_n;
        }
        acc.dl_mcs_sum += ue_report.dl_mcs.value();
        acc.ul_mcs_sum += ue_report.ul_mcs.value();
        ++acc.mcs_n;
        if (std::isfinite(ue_report.pusch_snr_db)) {
          acc.snr_sum += ue_report.pusch_snr_db;
          ++acc.snr_n;
        }
        if (ue_report.dl_bs == 0 or (slice_ml_target_rate > 0.0f and ue_report.dl_brate_kbps >= slice_ml_target_rate)) {
          ++acc.embb_ok;
        }
        if (ue_report.dl_bs == 0 and ue_report.bsr == 0) {
          ++acc.urllc_ok;
        }
        if (ue_report.max_sr_to_pusch_delay_ms.has_value()) {
          const float d        = *ue_report.max_sr_to_pusch_delay_ms;
          acc.sr_delay_max     = acc.has_sr_delay_max ? std::max(acc.sr_delay_max, d) : d;
          acc.has_sr_delay_max = true;
        }
        if (ue_report.avg_sr_to_pusch_delay_ms.has_value()) {
          acc.sr_delay_sum += *ue_report.avg_sr_to_pusch_delay_ms;
          ++acc.sr_delay_n;
        }
      }
    }
  }
  next_report->events.swap(pending_events);

  next_report->pci = cell_cfg.pci;
  // The window of slots for a report should be [start, stop) = [last_slot_tx + 1 - period, last_slot_tx + 1).
  // e.g. if the report period is 10, and we are at slot 0.9 (the last slot of the report), then the start slot is
  // 0.9 + 0.1 - 1.0 == 0.
  next_report->slot                  = last_slot_tx.without_hyper_sfn() + 1 - data.nof_slots;
  next_report->nof_slots             = data.nof_slots;
  next_report->nof_error_indications = data.error_indication_counter;
  next_report->average_decision_latency =
      next_report->nof_slots > 0 ? data.decision_latency_sum / next_report->nof_slots : std::chrono::microseconds{0};
  next_report->max_decision_latency      = data.max_decision_latency;
  next_report->max_decision_latency_slot = data.max_decision_latency_slot;
  next_report->latency_histogram         = data.decision_latency_hist;
  next_report->nof_prbs                  = cell_cfg.nof_dl_prbs; // TODO: to be removed from the report.
  next_report->nof_dl_slots              = data.nof_dl_slots;
  next_report->nof_ul_slots              = data.nof_ul_slots;
  next_report->nof_prach_preambles       = data.nof_prach_preambles;
  next_report->dl_grants_count           = data.nof_ue_pdsch_grants;
  next_report->ul_grants_count           = data.nof_ue_pusch_grants;
  next_report->nof_failed_pdcch_allocs   = data.nof_failed_pdcch_allocs;
  next_report->nof_failed_uci_allocs     = data.nof_failed_uci_allocs;
  next_report->nof_msg3_ok               = data.nof_msg3_ok;
  next_report->nof_msg3_nok              = data.nof_msg3_nok;
  next_report->avg_prach_delay_slots =
      data.nof_prach_preambles > 0
          ? std::optional{static_cast<float>(data.sum_prach_delay_slots) / static_cast<float>(data.nof_prach_preambles)}
          : std::nullopt;
  next_report->nof_failed_pdsch_allocs_late_harqs = data.nof_failed_pdsch_allocs_late_harqs;
  next_report->nof_failed_pusch_allocs_late_harqs = data.nof_failed_pusch_allocs_late_harqs;
  next_report->nof_filtered_events                = data.filtered_events_counter;
  // Note: PUCCH is only allocated on full UL slots.
  next_report->pucch_tot_rb_usage_avg =
      data.nof_ul_slots > 0 ? static_cast<float>(data.pucch_rbs_used) / data.nof_ul_slots : 0;
  if (cell_cfg.tdd_cfg_common.has_value()) {
    const float nof_tdd_periods_per_metric_report =
        static_cast<float>(next_report->nof_slots) /
        static_cast<float>(nof_slots_per_tdd_period(cell_cfg.tdd_cfg_common.value()));

    for (unsigned rb_count : ul_prbs_used_per_tdd_slot_idx) {
      const auto avg_nof_rbs =
          static_cast<unsigned>(std::round(static_cast<float>(rb_count) / nof_tdd_periods_per_metric_report));
      next_report->pusch_prbs_used_per_tdd_slot_idx.push_back(avg_nof_rbs);
    }

    for (unsigned rb_count : dl_prbs_used_per_tdd_slot_idx) {
      const auto avg_nof_rbs =
          static_cast<unsigned>(std::round(static_cast<float>(rb_count) / nof_tdd_periods_per_metric_report));
      next_report->pdsch_prbs_used_per_tdd_slot_idx.push_back(avg_nof_rbs);
    }
  }
  // Reset cell-wide metric counters.
  data = {};

  // Clear the PRB vectors for the next report.
  for (unsigned& rb_count : ul_prbs_used_per_tdd_slot_idx) {
    rb_count = 0;
  }
  for (unsigned& rb_count : dl_prbs_used_per_tdd_slot_idx) {
    rb_count = 0;
  }

  for (scheduler_slice_metrics& sm : last_slice_snapshot) {
    auto acc_it = slice_accums.find({sm.sst, sm.sd});
    if (acc_it == slice_accums.end()) {
      continue;
    }
    const slice_accum& acc = acc_it->second;
    sm.dl_brate_kbps_sum   = acc.dl_brate_sum;
    sm.ul_brate_kbps_sum   = acc.ul_brate_sum;
    sm.dl_bs_sum           = acc.dl_bs_sum;
    sm.bsr_sum             = acc.bsr_sum;
    sm.ssr_embb  = acc.nof_ues > 0 ? static_cast<float>(acc.embb_ok) / static_cast<float>(acc.nof_ues) : -1.0f;
    sm.ssr_urllc = acc.nof_ues > 0 ? static_cast<float>(acc.urllc_ok) / static_cast<float>(acc.nof_ues) : -1.0f;
  }

  if (slice_ml_dataset::is_slicemanager_enabled()) {
    const uint32_t cell_prbs = cell_cfg.nof_dl_prbs;
    for (const scheduler_slice_metrics& sm : last_slice_snapshot) {
      slice_ml_dataset::slice_sample ss{};
      ss.period_idx    = slice_ml_period;
      ss.slot          = slice_ml_slot;
      ss.period_ms     = slice_ml_period_ms;
      ss.cell_nof_prbs = cell_prbs;
      ss.pci           = static_cast<uint16_t>(cell_cfg.pci);
      ss.sst           = sm.sst;
      ss.sd            = sm.sd;
      ss.nof_ues       = sm.nof_ues;

      ss.min_prbs    = sm.min_prbs;
      ss.max_prbs    = sm.max_prbs;
      ss.ded_prbs    = sm.ded_prbs;
      ss.min_prbs_ul = sm.min_prbs_ul;
      ss.max_prbs_ul = sm.max_prbs_ul;
      ss.ded_prbs_ul = sm.ded_prbs_ul;

      const float prb_scale = cell_prbs > 0 ? 100.0f / static_cast<float>(cell_prbs) : 0.0f;
      ss.min_prb_ratio      = sm.min_prbs * prb_scale;
      ss.max_prb_ratio      = sm.max_prbs * prb_scale;
      ss.min_prb_ratio_ul   = sm.min_prbs_ul * prb_scale;
      ss.max_prb_ratio_ul   = sm.max_prbs_ul * prb_scale;

      ss.avg_dl_rbs_per_slot = sm.avg_dl_rbs_per_slot;
      ss.avg_ul_rbs_per_slot = sm.avg_ul_rbs_per_slot;
      ss.dl_prb_share = cell_prbs > 0 ? sm.avg_dl_rbs_per_slot / static_cast<float>(cell_prbs) : 0.0f;
      ss.ul_prb_share = cell_prbs > 0 ? sm.avg_ul_rbs_per_slot / static_cast<float>(cell_prbs) : 0.0f;

      ss.target_dl_rate_kbps = slice_ml_target_rate;
      ss.delay_budget_ms     = slice_ml_budget_ms;

      ss.dl_brate_kbps_sum = sm.dl_brate_kbps_sum;
      ss.ul_brate_kbps_sum = sm.ul_brate_kbps_sum;
      ss.dl_bs_sum         = sm.dl_bs_sum;
      ss.bsr_sum           = sm.bsr_sum;
      ss.ssr_embb          = sm.ssr_embb;
      ss.ssr_urllc         = sm.ssr_urllc;

      auto it = slice_accums.find({sm.sst, sm.sd});
      if (it != slice_accums.end()) {
        const slice_accum& acc = it->second;
        ss.dl_nof_ok_sum       = acc.dl_ok;
        ss.dl_nof_nok_sum      = acc.dl_nok;
        ss.ul_nof_ok_sum       = acc.ul_ok;
        ss.ul_nof_nok_sum      = acc.ul_nok;

        ss.has_mean_cqi = acc.cqi_n > 0;
        ss.mean_cqi     = ss.has_mean_cqi ? static_cast<float>(acc.cqi_sum / acc.cqi_n) : 0.0f;
        ss.has_mean_dl_mcs = acc.mcs_n > 0;
        ss.mean_dl_mcs     = ss.has_mean_dl_mcs ? static_cast<float>(acc.dl_mcs_sum / acc.mcs_n) : 0.0f;
        ss.has_mean_ul_mcs = acc.mcs_n > 0;
        ss.mean_ul_mcs     = ss.has_mean_ul_mcs ? static_cast<float>(acc.ul_mcs_sum / acc.mcs_n) : 0.0f;
        ss.has_mean_pusch_snr_db = acc.snr_n > 0;
        ss.mean_pusch_snr_db     = ss.has_mean_pusch_snr_db ? static_cast<float>(acc.snr_sum / acc.snr_n) : 0.0f;

        ss.has_mean_sr_to_pusch_delay_ms = acc.sr_delay_n > 0;
        ss.mean_sr_to_pusch_delay_ms =
            ss.has_mean_sr_to_pusch_delay_ms ? static_cast<float>(acc.sr_delay_sum / acc.sr_delay_n) : 0.0f;
        ss.has_max_sr_to_pusch_delay_ms = acc.has_sr_delay_max;
        ss.max_sr_to_pusch_delay_ms     = acc.sr_delay_max;
      }
      ss.has_power_stats                 = sm.has_power_stats;
      ss.power_committed_w               = sm.power_committed_w;
      ss.power_remaining_w               = sm.power_remaining_w;
      ss.power_pairs_rejected_unit_taken = sm.power_pairs_rejected_unit_taken;
      ss.power_pairs_rejected_infeasible = sm.power_pairs_rejected_infeasible;
      ss.power_pairs_rejected_no_demand  = sm.power_pairs_rejected_no_demand;
      ss.power_decode_steps_taken        = sm.power_decode_steps_taken;
      ss.power_decode_steps_total        = sm.power_decode_steps_total;

      slice_ml_dataset::log_slice_sample(ss);
    }
  }

  next_report->slice_metrics = std::move(last_slice_snapshot);
  last_slice_snapshot.clear();

  // Report all UE metrics in a batch.
  // Note: next_report will be reset afterwards. However, we prefer to first commit before fetching a new report.
  next_report.reset();
}

void cell_metrics_handler::handle_slot_result(slot_point_extended       sl_tx,
                                              const sched_result&       slot_result,
                                              std::chrono::microseconds slot_decision_latency)
{
  if (OCUDU_UNLIKELY(not last_slot_tx.valid())) {
    data.nof_slots = 1;
  } else {
    data.nof_slots += sl_tx - last_slot_tx;
  }
  last_slot_tx = sl_tx;

  data.nof_ue_pdsch_grants += slot_result.dl.ue_grants.size();
  for (const dl_msg_alloc& dl_grant : slot_result.dl.ue_grants) {
    auto it = rnti_to_ue_index_lookup.find(dl_grant.pdsch_cfg.rnti);
    if (it == rnti_to_ue_index_lookup.end()) {
      // UE not found.
      continue;
    }
    ue_metric_context& u = ues[it->second];
    for (const auto& cw : dl_grant.pdsch_cfg.codewords) {
      u.data.dl_mcs += cw.mcs_index.value();
      ++u.data.nof_dl_cws;
    }

    unsigned grant_prbs;
    if (dl_grant.pdsch_cfg.rbs.is_type0()) {
      grant_prbs = convert_rbgs_to_prbs(dl_grant.pdsch_cfg.rbs.type0(),
                                        {0, cell_cfg.nof_dl_prbs},
                                        get_nominal_rbg_size(cell_cfg.nof_dl_prbs, true))
                       .count();
    } else {
      grant_prbs = (dl_grant.pdsch_cfg.rbs.type1().length());
    }
    u.data.tot_dl_prbs_used += grant_prbs;
    if (not dl_prbs_used_per_tdd_slot_idx.empty()) {
      dl_prbs_used_per_tdd_slot_idx[last_slot_tx.count() % dl_prbs_used_per_tdd_slot_idx.size()] += grant_prbs;
    }
    u.last_dl_olla = dl_grant.context.olla_offset;
    if (u.data.last_pdsch_slot.valid()) {
      u.data.max_pdsch_distance_slots =
          std::max(static_cast<unsigned>(last_slot_tx.without_hyper_sfn() - u.data.last_pdsch_slot),
                   u.data.max_pdsch_distance_slots);
    }
    u.data.last_pdsch_slot = last_slot_tx.without_hyper_sfn();
  }

  data.nof_ue_pusch_grants += slot_result.ul.puschs.size();
  for (const ul_sched_info& ul_grant : slot_result.ul.puschs) {
    auto it = rnti_to_ue_index_lookup.find(ul_grant.pusch_cfg.rnti);
    if (it == rnti_to_ue_index_lookup.end()) {
      // UE not found.
      continue;
    }
    unsigned grant_prbs;
    if (ul_grant.pusch_cfg.rbs.is_type0()) {
      grant_prbs = convert_rbgs_to_prbs(ul_grant.pusch_cfg.rbs.type0(),
                                        {0, cell_cfg.nof_dl_prbs},
                                        get_nominal_rbg_size(cell_cfg.nof_dl_prbs, true))
                       .count();
    } else {
      grant_prbs = (ul_grant.pusch_cfg.rbs.type1().length());
    }
    ues[it->second].data.tot_ul_prbs_used += grant_prbs;
    if (ul_prbs_used_per_tdd_slot_idx.size()) {
      ul_prbs_used_per_tdd_slot_idx[last_slot_tx.count() % ul_prbs_used_per_tdd_slot_idx.size()] += grant_prbs;
    }
    ue_metric_context& u = ues[it->second];
    u.data.ul_mcs += ul_grant.pusch_cfg.mcs_index.value();
    u.last_ul_olla = ul_grant.context.olla_offset;
    if (u.data.last_sr_slot.valid()) {
      unsigned sr_to_pusch_delay = last_slot_tx.without_hyper_sfn() - u.data.last_sr_slot;
      u.data.sum_sr_to_pusch_delay_slots += sr_to_pusch_delay;
      u.data.max_sr_to_pusch_delay_slots = std::max(sr_to_pusch_delay, u.data.max_sr_to_pusch_delay_slots);
      u.data.last_sr_slot.clear();
      u.data.count_handled_sr++;
    }
    ++u.data.nof_puschs;
    if (u.data.last_pusch_slot.valid()) {
      u.data.max_pusch_distance_slots =
          std::max(static_cast<unsigned>(last_slot_tx.without_hyper_sfn() - u.data.last_pusch_slot),
                   u.data.max_pusch_distance_slots);
    }
    u.data.last_pusch_slot = last_slot_tx.without_hyper_sfn();

    // TS 38.314 §4.2.1.2.2, TS 38.321 §5.4.5
    if (bsr_ml_dataset::is_enabled()) {
      const slot_point grant_slot = last_slot_tx.without_hyper_sfn();
      bsr_ml_dataset::record_ul_grant(static_cast<uint16_t>(it->second),
                                      static_cast<uint8_t>(ul_grant.pusch_cfg.harq_id),
                                      grant_slot.system_slot(),
                                      ul_grant.pusch_cfg.new_data);
    }
  }

  // PUCCH resource usage.
  prb_bitmap pucch_prbs(cell_cfg.nof_ul_prbs);
  for (const auto& pucch : slot_result.ul.pucchs) {
    // Mark the PRBs used by this PUCCH.
    pucch_prbs.fill(pucch.resources.prbs.start(), pucch.resources.prbs.stop());
    pucch_prbs.fill(pucch.resources.second_hop_prbs.start(), pucch.resources.second_hop_prbs.stop());
  }
  data.pucch_rbs_used += pucch_prbs.count();

  // Count DL and UL slots.
  data.nof_dl_slots += slot_result.dl.nof_dl_symbols > 0;
  data.nof_ul_slots += slot_result.ul.nof_ul_symbols > 0;

  // Process latency.
  data.decision_latency_sum += slot_decision_latency;
  if (data.max_decision_latency < slot_decision_latency) {
    data.max_decision_latency      = slot_decision_latency;
    data.max_decision_latency_slot = last_slot_tx.without_hyper_sfn();
  }
  unsigned bin_idx = slot_decision_latency.count() / scheduler_cell_metrics::nof_usec_per_bin;
  bin_idx          = std::min(bin_idx, scheduler_cell_metrics::latency_hist_bins - 1);
  ++data.decision_latency_hist[bin_idx];

  // Failed allocation attempts.
  data.nof_failed_pdcch_allocs += slot_result.failed_attempts.pdcch;
  data.nof_failed_uci_allocs += slot_result.failed_attempts.uci;
}

void cell_metrics_handler::push_result(slot_point_extended       sl_tx,
                                       const sched_result&       slot_result,
                                       std::chrono::microseconds slot_decision_latency)
{
  if (not enabled()) {
    return;
  }

  handle_slot_result(sl_tx, slot_result, slot_decision_latency);

  if (notifier.is_sched_report_required(sl_tx)) {
    // Prepare report and forward it to the notifier.
    report_metrics();
  }
}

void cell_metrics_handler::handle_cell_deactivation()
{
  // Commit whatever is pending for the report.
  report_metrics();
  last_slot_tx = {};
}

scheduler_ue_metrics
cell_metrics_handler::ue_metric_context::compute_report(std::chrono::milliseconds metric_report_period,
                                                        unsigned                  slots_per_sf)
{
  auto convert_slots_to_ms = [slots_per_sf](unsigned slots) {
    return static_cast<float>(slots) / static_cast<float>(slots_per_sf);
  };
  scheduler_ue_metrics ret{};
  ret.ue_index            = ue_index;
  ret.pci                 = pci;
  ret.rnti                = rnti;
  ret.s_nssai             = s_nssai;
  ret.cqi_stats           = data.cqi;
  ret.dl_ri_stats         = data.dl_ri;
  uint8_t mcs             = data.nof_dl_cws > 0 ? std::round(static_cast<float>(data.dl_mcs) / data.nof_dl_cws) : 0;
  ret.dl_mcs              = sch_mcs_index{mcs};
  mcs                     = data.nof_puschs > 0 ? std::round(static_cast<float>(data.ul_mcs) / data.nof_puschs) : 0;
  ret.ul_mcs              = sch_mcs_index{mcs};
  ret.tot_pdsch_prbs_used = data.tot_dl_prbs_used;
  ret.tot_pusch_prbs_used = data.tot_ul_prbs_used;
  ret.dl_brate_kbps       = static_cast<double>(data.sum_dl_tb_bytes * 8U) / metric_report_period.count();
  ret.ul_brate_kbps       = static_cast<double>(data.sum_ul_tb_bytes * 8U) / metric_report_period.count();
  ret.dl_nof_ok           = data.count_uci_harq_acks;
  ret.dl_nof_nok          = data.count_uci_harqs - data.count_uci_harq_acks;
  ret.ul_nof_ok           = data.count_crc_acks;
  ret.ul_nof_nok          = data.count_crc_pdus - data.count_crc_acks;
  ret.pusch_snr_db        = data.nof_pusch_snr_reports > 0 ? data.sum_pusch_snrs / data.nof_pusch_snr_reports : 0;
  ret.pusch_rsrp_db       = data.nof_pusch_rsrp_reports > 0 ? data.sum_pusch_rsrp / data.nof_pusch_rsrp_reports
                                                            : -std::numeric_limits<float>::infinity();
  ret.ul_ri_stats         = data.ul_ri;
  ret.pucch_snr_db        = data.nof_pucch_snr_reports > 0 ? data.sum_pucch_snrs / data.nof_pucch_snr_reports : 0;
  ret.last_dl_olla        = last_dl_olla;
  ret.last_ul_olla        = last_ul_olla;
  ret.bsr                 = last_bsr;
  ret.sr_count            = data.count_sr;
  ret.dl_bs               = 0;
  for (const unsigned value : last_dl_bs) {
    ret.dl_bs += value;
  }
  ret.ta_stats                       = data.ta;
  ret.pusch_ta_stats                 = data.pusch_ta;
  ret.pucch_ta_stats                 = data.pucch_ta;
  ret.srs_ta_stats                   = data.srs_ta;
  ret.last_phr                       = last_phr;
  ret.max_pdsch_distance_ms          = convert_slots_to_ms(data.max_pdsch_distance_slots);
  ret.max_pusch_distance_ms          = convert_slots_to_ms(data.max_pusch_distance_slots);
  ret.nof_pucch_f0f1_invalid_harqs   = data.nof_pucch_f0f1_invalid_harqs;
  ret.nof_pucch_f2f3f4_invalid_harqs = data.nof_pucch_f2f3f4_invalid_harqs;
  ret.nof_pucch_f2f3f4_invalid_harqs = data.nof_pucch_f2f3f4_invalid_harqs;
  ret.nof_pucch_f2f3f4_invalid_csis  = data.nof_pucch_f2f3f4_invalid_csis;
  ret.nof_pusch_invalid_harqs        = data.nof_pusch_invalid_harqs;
  ret.nof_pusch_invalid_csis         = data.nof_pusch_invalid_csis;
  if (data.nof_ul_ces > 0) {
    ret.avg_ce_delay_ms = convert_slots_to_ms(data.sum_ul_ce_delay_slots) / static_cast<float>(data.nof_ul_ces);
    ret.max_ce_delay_ms = convert_slots_to_ms(data.max_ul_ce_delay_slots);
  }
  if (data.count_crc_pdus > 0) {
    ret.avg_crc_delay_ms = convert_slots_to_ms(data.sum_crc_delay_slots) / static_cast<float>(data.count_crc_pdus);
    ret.max_crc_delay_ms = convert_slots_to_ms(data.max_crc_delay_slots);
  }
  if (data.count_pusch_harq_pdus > 0) {
    ret.avg_pusch_harq_delay_ms =
        convert_slots_to_ms(data.sum_pusch_harq_delay_slots) / static_cast<float>(data.count_pusch_harq_pdus);
    ret.max_pusch_harq_delay_ms = convert_slots_to_ms(data.max_pusch_harq_delay_slots);
  }
  if (data.count_pucch_harq_pdus > 0) {
    ret.avg_pucch_harq_delay_ms =
        convert_slots_to_ms(data.sum_pucch_harq_delay_slots) / static_cast<float>(data.count_pucch_harq_pdus);
    ret.max_pucch_harq_delay_ms = convert_slots_to_ms(data.max_pucch_harq_delay_slots);
  }
  if (data.count_handled_sr > 0) {
    ret.avg_sr_to_pusch_delay_ms =
        convert_slots_to_ms(data.sum_sr_to_pusch_delay_slots) / static_cast<float>(data.count_handled_sr);
    ret.max_sr_to_pusch_delay_ms = convert_slots_to_ms(data.max_sr_to_pusch_delay_slots);
  }

  ret.recommended_periodic_bsr_timer = bsr_ml_dataset::get_recommended_periodic_bsr_timer(static_cast<uint16_t>(ue_index));

  // Reset UE stats metrics on every report.
  reset();

  return ret;
}

void cell_metrics_handler::ue_metric_context::reset()
{
  // Note: for BSR and CQI we just keep the last without resetting the value at every slot.
  data = {};
}

cell_metrics_handler* scheduler_metrics_handler::add_cell(
    const cell_configuration&                                                      cell_cfg,
    const std::optional<sched_cell_configuration_request_message::metrics_config>& metrics_cfg)
{
  if (cells.contains(cell_cfg.cell_index)) {
    ocudulog::fetch_basic_logger("SCHED").warning("Cell={} already exists", fmt::underlying(cell_cfg.cell_index));
    return nullptr;
  }

  cells.emplace(cell_cfg.cell_index, std::make_unique<cell_metrics_handler>(cell_cfg, metrics_cfg));

  return cells[cell_cfg.cell_index].get();
}

void scheduler_metrics_handler::rem_cell(du_cell_index_t cell_index)
{
  cells.erase(cell_index);
}
