// SPDX-FileCopyrightText: Copyright (C) 2021-2026 Software Radio Systems Limited
// SPDX-License-Identifier: BSD-3-Clause-Open-MPI
// Portions of this file may implement 3GPP specifications, which may be subject to additional licensing requirements.

#include "ue_cell.h"
#include "../logging/ml_la_dataset_logger.h"
#include "../support/dmrs_helpers.h"
#include "../support/mcs_calculator.h"
#include "../support/mcs_ml_predictor.h"
#include "../support/pdcch_aggregation_level_calculator.h"
#include "../support/sch_pdu_builder.h"
#include "ue_drx_controller.h"
#include "ocudu/ocudulog/ocudulog.h"
#include "ocudu/ran/sch/tbs_calculator.h"
#include "ocudu/scheduler/scheduler_feedback_handler.h"
#include <limits>

using namespace ocudu;

/// The default number of HARQ processes to be used on the PDSCH of a serving cell. See TS 38.331, \c
/// nrofHARQ-ProcessesForPDSCH.
static constexpr unsigned DEFAULT_NOF_DL_HARQS = 8;

/// The default number of HARQ processes to be used on the PUSCH of a serving cell. See TS 38.331, \c
/// nrofHARQ-ProcessesForPUSCH.
static constexpr unsigned DEFAULT_NOF_UL_HARQS = 16;

ue_cell::ue_cell(du_ue_index_t                ue_index_,
                 rnti_t                       crnti_val,
                 serv_cell_index_t            serv_cell_index_,
                 const ue_cell_configuration& ue_cell_cfg_,
                 cell_harq_manager&           cell_harq_pool,
                 ue_shared_context            shared_ctx_,
                 const ue_cell_components&    components_,
                 std::optional<slot_point>    msg3_slot_rx,
                 ocudulog::basic_logger&      logger_) :
  ue_index(ue_index_),
  cell_index(ue_cell_cfg_.cell_cfg_common.cell_index),
  harqs(
      cell_harq_pool.add_ue(ue_index,
                            crnti_val,
                            ue_cell_cfg_.pdsch_serving_cell_cfg() != nullptr
                                ? std::min(static_cast<unsigned>(ue_cell_cfg_.pdsch_serving_cell_cfg()->nof_harq_proc),
                                           static_cast<unsigned>(MAX_NOF_HARQS_NON_NTN))
                                : DEFAULT_NOF_DL_HARQS,
                            DEFAULT_NOF_UL_HARQS)),
  crnti_(crnti_val),
  cell_cfg(ue_cell_cfg_.cell_cfg_common),
  ue_cfg(&ue_cell_cfg_),
  expert_cfg(cell_cfg.expert_cfg.ue),
  shared_ctx(shared_ctx_),
  components(components_),
  logger(logger_)
{
  if (serv_cell_index_ == SERVING_PCELL_IDX) {
    // Set ConRes procedure complete by default. Variable only needed for RACHs where MSG3 contains ConRes MAC-CE.
    pcell_state.emplace(ue_pcell_state{});
    pcell_state->conres_complete = true;
    if (msg3_slot_rx.has_value()) {
      pcell_state->msg3_rx_slot = msg3_slot_rx.value();
    }
  }
}

void ue_cell::deactivate()
{
  // Stop HARQ retransmissions.
  // Note: We assume that when this function is called, any RRC container (e.g. containing RRC Release) was already
  // transmitted+ACKed (ensured by F1AP).
  harqs.cancel_retxs();

  // Mark the UE as inactive.
  active = false;
}

void ue_cell::handle_reconfiguration_request(const ue_cell_configuration& ue_cell_cfg)
{
  ue_cfg = &ue_cell_cfg;

  // Cancel HARQ retxs, given that the UE may have changed some critical params (e.g. MCS tables)
  harqs.cancel_retxs();

  // Reconfigure HARQ entity and increase nof HARQ processes if needed.
  unsigned new_dl_harqs = ue_cell_cfg.pdsch_serving_cell_cfg() != nullptr
                              ? static_cast<unsigned>(ue_cell_cfg.pdsch_serving_cell_cfg()->nof_harq_proc)
                              : harqs.nof_dl_harqs();
  unsigned new_ul_harqs = ue_cell_cfg.pusch_serving_cell_cfg() != nullptr
                              ? static_cast<unsigned>(ue_cell_cfg.pusch_serving_cell_cfg()->nof_harq_proc)
                              : harqs.nof_ul_harqs();

  harqs.reconfigure(new_dl_harqs,
                    new_ul_harqs,
                    ue_cell_cfg.pdsch_serving_cell_cfg()->dl_harq_feedback_disabled,
                    ue_cell_cfg.pusch_serving_cell_cfg()->ul_harq_mode);

  get_pusch_power_controller().reconfigure(ue_cell_cfg);
}

void ue_cell::set_fallback_state(bool set_fallback, bool is_reconfig, bool reestablished)
{
  ocudu_assert(pcell_state.has_value(),
               "ue={} rnti={}: Cannot set fallback state on non-Pcell",
               fmt::underlying(ue_index),
               rnti());

  // In case of Pcell, update reconf_ongoing state.
  pcell_state->reconf_ongoing = is_reconfig;
  pcell_state->reestablished  = reestablished;
  if (pcell_state->in_fallback_mode == set_fallback) {
    // No state change.
    return;
  }
  pcell_state->in_fallback_mode = set_fallback;

  // Cancel pending HARQs retxs of different state.
  harqs.cancel_retxs();
  logger.debug("ue={} rnti={}: {} fallback mode",
               fmt::underlying(ue_index),
               rnti(),
               pcell_state->in_fallback_mode ? "Entering" : "Leaving");
}

std::optional<dl_harq_process_handle> ue_cell::handle_dl_ack_info(slot_point                 uci_slot,
                                                                  mac_harq_ack_report_status ack_value,
                                                                  unsigned                   harq_bit_idx,
                                                                  std::optional<float>       pucch_snr)
{
  std::optional<dl_harq_process_handle> h_dl = harqs.find_dl_harq_waiting_ack(uci_slot, harq_bit_idx);
  if (not h_dl.has_value()) {
    logger.warning("rnti={}: Discarding ACK info. Cause: DL HARQ for uci slot={} and HARQ-ACK bit={} not found.",
                   rnti(),
                   uci_slot,
                   harq_bit_idx);
    return h_dl;
  }

  // Update HARQ state.
  const bool ret = h_dl->dl_ack_info(ack_value, pucch_snr);
  if (not ret) {
    // HARQ not in a valid state to be ACKed/NACKed.
    h_dl.reset();
    return h_dl;
  }

  // In case of NACK/DTX, extend DRX window, if needed.
  if (ack_value != mac_harq_ack_report_status::ack) {
    shared_ctx.drx_ctrl.on_dl_harq_nack(uci_slot);
  }

  // If the HARQ report was DTX, do not forward the feeback to the link adaptation controller, as the issue is not
  // necessarily in the PDSCH MCS.
  if (ack_value != mac_harq_ack_report_status::dtx) {
    components.ue_mcs_calculator->handle_dl_ack_info(ack_value == mac_harq_ack_report_status::ack,
                                                     h_dl->get_grant_params().mcs,
                                                     h_dl->get_grant_params().mcs_table,
                                                     h_dl->get_grant_params().olla_mcs);
  }

  return h_dl;
}

expected<units::bytes> ue_cell::handle_crc_pdu(slot_point pusch_slot, const ul_crc_pdu_indication& crc_pdu)
{
  // Find UL HARQ with matching PUSCH slot.
  std::optional<ul_harq_process_handle> h_ul = harqs.find_ul_harq_waiting_ack(pusch_slot);
  if (not h_ul.has_value() or h_ul->id() != crc_pdu.harq_id) {
    logger.warning("rnti={} h_id={}: Discarding CRC. Cause: UL HARQ process is not expecting CRC for PUSCH slot {}",
                   rnti(),
                   fmt::underlying(crc_pdu.harq_id),
                   pusch_slot);
    return make_unexpected(default_error_t{});
  }

  // Update UL HARQ state.
  auto tbs_ret = h_ul->ul_crc_info(crc_pdu.tb_crc_success);

  if (tbs_ret.has_value()) {
    // HARQ with matching ID and UCI slot was found.

    // Update link adaptation controller.
    components.ue_mcs_calculator->handle_ul_crc_info(crc_pdu.tb_crc_success,
                                                     h_ul->get_grant_params().mcs,
                                                     h_ul->get_grant_params().mcs_table,
                                                     h_ul->get_grant_params().olla_mcs);

    // Update PUSCH SNR reported from PHY.
    if (crc_pdu.ul_sinr_dB.has_value()) {
      components.channel_state->update_pusch_snr(crc_pdu.ul_sinr_dB.value());
    }

    if (ml_la_dataset::is_enabled()) {
      const auto&             gp = h_ul->get_grant_params();
      ml_la_dataset::ul_sample s{};
      s.slot        = pusch_slot.system_slot();
      s.rnti        = static_cast<uint16_t>(rnti());
      s.ue_index    = static_cast<uint16_t>(ue_index);
      s.harq_id     = static_cast<uint8_t>(crc_pdu.harq_id);
      s.nof_retxs   = static_cast<uint8_t>(h_ul->nof_retxs());
      s.mcs         = gp.mcs.value();
      s.mcs_table   = static_cast<uint8_t>(gp.mcs_table);
      s.tbs_bytes   = static_cast<uint32_t>(gp.tbs.value());
      s.nof_prbs    = static_cast<uint16_t>(gp.rbs.is_type1() ? gp.rbs.type1().length() : gp.rbs.type0().count());
      s.nof_symbols = gp.nof_symbols;
      s.nof_layers  = gp.nof_layers;
      s.olla_mcs    = gp.olla_mcs.has_value() ? static_cast<int16_t>(gp.olla_mcs->value()) : static_cast<int16_t>(-1);

      const auto& cs      = channel_state_manager();
      s.wideband_cqi      = static_cast<uint8_t>(cs.get_wideband_cqi().value());
      s.pusch_snr_db      = cs.get_pusch_snr();
      s.pusch_avg_sinr_db = cs.get_pusch_average_sinr();
      s.ul_snr_offset_db  = link_adaptation_controller().ul_snr_offset_db();

      s.ul_sinr_db   = crc_pdu.ul_sinr_dB.has_value() ? crc_pdu.ul_sinr_dB.value() : std::numeric_limits<float>::quiet_NaN();
      s.ul_rsrp_dbfs = crc_pdu.ul_rsrp_dBFS.has_value() ? crc_pdu.ul_rsrp_dBFS.value() : std::numeric_limits<float>::quiet_NaN();
      s.ta_ns        = crc_pdu.time_advance_offset.has_value()
                           ? static_cast<int32_t>(crc_pdu.time_advance_offset.value().to_seconds() * 1e9)
                           : INT32_MIN;

      s.nof_dl_layers                           = static_cast<uint8_t>(cs.get_nof_dl_layers());
      s.nof_ul_layers                           = static_cast<uint8_t>(cs.get_nof_ul_layers());
      const std::optional<csi_report_data>& csi = cs.get_latest_csi_report();
      s.csi_ri  = (csi.has_value() and csi->ri.has_value()) ? static_cast<int16_t>(csi->ri->value()) : int16_t{-1};
      s.csi_cri = (csi.has_value() and csi->cri.has_value()) ? static_cast<int16_t>(csi->cri.value()) : int16_t{-1};
      s.slots_since_srs =
          cs.last_aperiodic_srs_slot.valid() ? static_cast<int32_t>(pusch_slot - cs.last_aperiodic_srs_slot) : -1;

      const auto& lac    = link_adaptation_controller();
      s.dl_cqi_offset_db = lac.dl_cqi_offset();
      s.effective_cqi    = lac.get_effective_cqi();
      s.ul_olla_enabled  = lac.is_ul_olla_enabled() ? 1 : 0;
      s.dl_olla_enabled  = lac.is_dl_olla_enabled() ? 1 : 0;

      s.ul_bytes_in_harq   = static_cast<uint32_t>(harqs.total_ul_bytes_waiting_ack().value());
      s.nof_empty_ul_harqs = static_cast<uint16_t>(harqs.nof_empty_ul_harqs());
      s.nof_ul_harqs       = static_cast<uint16_t>(harqs.nof_ul_harqs());
      s.max_nof_ul_retxs   = static_cast<uint8_t>(h_ul->max_nof_retxs());
      s.ndi                = h_ul->ndi() ? 1 : 0;

      s.slice_id     = gp.slice_id.has_value() ? static_cast<int16_t>(gp.slice_id->value()) : int16_t{-1};
      s.dci_cfg_type = static_cast<uint8_t>(gp.dci_cfg_type);

      s.sfn           = static_cast<uint16_t>(pusch_slot.sfn());
      s.slot_in_frame = static_cast<uint8_t>(pusch_slot.slot_index());
      s.subframe      = static_cast<uint8_t>(pusch_slot.subframe_index());
      s.numerology    = static_cast<uint8_t>(pusch_slot.numerology());

      s.is_fallback = is_in_fallback_mode() ? 1 : 0;

      const mcs_ml::predictor& ml = mcs_ml::predictor::instance();
      s.ml_in_control             = ml.enabled() ? 1 : 0;
      s.ml_p_succ                 = static_cast<float>(ml.predict_for(s.mcs,
                                                                      s.mcs_table,
                                                                      s.wideband_cqi,
                                                                      s.pusch_avg_sinr_db,
                                                                      s.ul_snr_offset_db,
                                                                      s.olla_mcs));

      s.crc_success = crc_pdu.tb_crc_success ? 1 : 0;
      ml_la_dataset::log_ul_sample(s);
    }
  }

  return tbs_ret;
}

void ue_cell::handle_srs_channel_matrix(const srs_channel_matrix& channel_matrix)
{
  components.channel_state->update_srs_channel_matrix(channel_matrix, ue_cfg->get_pusch_codebook_config());
}

void ue_cell::handle_csi_report(const csi_report_data& csi_report)
{
  apply_link_adaptation_procedures(csi_report);
  if (not components.channel_state->handle_csi_report(csi_report)) {
    logger.warning("ue={} rnti={}: Invalid CSI report received", fmt::underlying(ue_index), rnti());
  }

  if (csi_report.valid and csi_report.first_tb_wideband_cqi == csi_report_data::wideband_cqi_type{0}) {
    // CQI==0 means that the UE is out-of-reach. We stop transmissions for this UE until its CQI becomes positive again.
    harqs.cancel_retxs();
  }
}

template <typename FilterSearchSpace>
static static_vector<const search_space_info*, MAX_NOF_SEARCH_SPACE_PER_BWP>
get_prioritized_search_spaces(const ue_cell& ue_cc, FilterSearchSpace filter, bool is_dl)
{
  static_vector<const search_space_info*, MAX_NOF_SEARCH_SPACE_PER_BWP> active_search_spaces;

  // Get all Search Spaces configured in PDCCH-Config for active BWP.
  const auto& bwp_ss_lst = ue_cc.cfg().bwp(ue_cc.active_bwp_id()).search_spaces;
  for (const search_space_config_ptr& ss : bwp_ss_lst) {
    const search_space_info& ss_info = ue_cc.cfg().search_space(ss->get_id());
    if (filter(ss_info)) {
      active_search_spaces.push_back(&ss_info);
    }
  }

  // Sort search spaces by priority.
  auto sort_ss = [&ue_cc, is_dl](const search_space_info* lhs, const search_space_info* rhs) {
    // NOTE: It does not matter whether we use lhs or rhs SearchSpace to get the aggregation level as we are sorting not
    // filtering. Filtering is already done in previous step.
    const unsigned aggr_lvl_idx = to_aggregation_level_index(
        ue_cc.get_aggregation_level(ue_cc.link_adaptation_controller().get_effective_cqi(), *lhs, is_dl));
    return lhs->cfg->get_nof_candidates()[aggr_lvl_idx] > rhs->cfg->get_nof_candidates()[aggr_lvl_idx];
  };
  std::sort(active_search_spaces.begin(), active_search_spaces.end(), sort_ss);

  return active_search_spaces;
}

static_vector<const search_space_info*, MAX_NOF_SEARCH_SPACE_PER_BWP>
ue_cell::get_active_dl_search_spaces(slot_point                             pdcch_slot,
                                     std::optional<dci_dl_rnti_config_type> required_dci_rnti_type) const
{
  static_vector<const search_space_info*, MAX_NOF_SEARCH_SPACE_PER_BWP> active_search_spaces;

  if (required_dci_rnti_type == dci_dl_rnti_config_type::tc_rnti_f1_0) {
    // In case of TC-RNTI, use Type-1 PDCCH CSS for a UE.
    active_search_spaces.push_back(
        &cfg().search_space(cfg().cell_cfg_common.dl_cfg_common.init_dl_bwp.pdcch_common.ra_search_space_id));
    return active_search_spaces;
  }

  // In fallback mode state, only use search spaces configured in CellConfigCommon.
  if (is_in_fallback_mode()) {
    ocudu_assert(not required_dci_rnti_type.has_value() or
                     required_dci_rnti_type == dci_dl_rnti_config_type::c_rnti_f1_0,
                 "Invalid required dci-rnti parameter");
    for (const search_space_configuration& ss :
         ue_cfg->cell_cfg_common.dl_cfg_common.init_dl_bwp.pdcch_common.search_spaces) {
      if (pdcch_helper::is_pdcch_monitoring_active(pdcch_slot, ss)) {
        active_search_spaces.push_back(&ue_cfg->search_space(ss.get_id()));
      }
    }
    return active_search_spaces;
  }

  auto filter_ss = [this, pdcch_slot, required_dci_rnti_type](const search_space_info& ss) {
    // See TS 38.213, A UE monitors PDCCH candidates in one or more of the following search spaces sets:
    // - a Type3-PDCCH CSS set configured by SearchSpace in PDCCH-Config with searchSpaceType = common for DCI formats
    //   with CRC scrambled by INT-RNTI, SFI-RNTI, TPC-PUSCH-RNTI, TPC-PUCCH-RNTI, or TPC-SRS-RNTI and, only for the
    //   primary cell, C-RNTI, MCS-C-RNTI, or CS-RNTI(s).
    // - a USS set configured by SearchSpace in PDCCH-Config with searchSpaceType = ue-Specific for DCI formats
    //   with CRC scrambled by C-RNTI, MCS-C-RNTI, SP-CSI-RNTI, or CS-RNTI(s).
    //
    // As per TS 38.213, the UE monitors PDCCH candidates for DCI format 0_0 and DCI format 1_0 with CRC scrambled by
    // the C-RNTI, the MCS-C-RNTI, or the CS-RNTI in the one or more search space sets in a slot where the UE monitors
    // PDCCH candidates for at least a DCI format 0_0 or a DCI format 1_0 with CRC scrambled by SI-RNTI, RA-RNTI or
    // P-RNTI.
    if (ss.cfg->is_common_search_space()) {
      const auto& pdcch_config_ss_lst = cfg().bwp(active_bwp_id()).dl_ded.value()->pdcch_cfg->search_spaces;
      const bool  is_type3_css        = std::find_if(pdcch_config_ss_lst.begin(),
                                             pdcch_config_ss_lst.end(),
                                             [&ss](const search_space_configuration& ss_cfg) {
                                               return ss.cfg->get_id() == ss_cfg.get_id();
                                             }) != pdcch_config_ss_lst.end();

      const bool is_ss_for_ra =
          ss.cfg->get_id() == cfg().cell_cfg_common.dl_cfg_common.init_dl_bwp.pdcch_common.ra_search_space_id;
      const bool is_ss_for_paging =
          not cfg().cell_cfg_common.dl_cfg_common.init_dl_bwp.pdcch_common.paging_search_space_id.has_value() or
          ss.cfg->get_id() ==
              cfg().cell_cfg_common.dl_cfg_common.init_dl_bwp.pdcch_common.paging_search_space_id.value();

      // [Implementation-defined] We exclude SearchSpace#0 to avoid the complexity of computing the SearchSpace#0 PDCCH
      // candidates monitoring occasions associated with a SS/PBCH block as mentioned in TS 38.213, clause 10.1.
      if (ss.cfg->get_id() == to_search_space_id(0) or
          (not is_ss_for_ra and not is_ss_for_paging and not is_type3_css)) {
        return false;
      }
    }

    if (not pdcch_helper::is_pdcch_monitoring_active(pdcch_slot, *ss.cfg)) {
      return false;
    }

    if (ss.get_pdcch_candidates(get_aggregation_level(link_adaptation_controller().get_effective_cqi(), ss, true),
                                pdcch_slot)
            .empty()) {
      return false;
    }
    if (required_dci_rnti_type.has_value() and
        not pdcch_helper::search_space_supports_dci_dl_format(*ss.cfg, get_dci_format(*required_dci_rnti_type))) {
      return false;
    }
    return true;
  };
  return get_prioritized_search_spaces(*this, filter_ss, true);
}

static_vector<const search_space_info*, MAX_NOF_SEARCH_SPACE_PER_BWP>
ue_cell::get_active_ul_search_spaces(slot_point                             pdcch_slot,
                                     std::optional<dci_ul_rnti_config_type> required_dci_rnti_type) const
{
  // In fallback mode state, only use search spaces configured in CellConfigCommon.
  if (is_in_fallback_mode()) {
    static_vector<const search_space_info*, MAX_NOF_SEARCH_SPACE_PER_BWP> active_search_spaces;
    ocudu_assert(not required_dci_rnti_type.has_value() or
                     required_dci_rnti_type == dci_ul_rnti_config_type::c_rnti_f0_0,
                 "Invalid required dci-rnti parameter");
    for (const search_space_configuration& ss :
         ue_cfg->cell_cfg_common.dl_cfg_common.init_dl_bwp.pdcch_common.search_spaces) {
      if (pdcch_helper::is_pdcch_monitoring_active(pdcch_slot, ss)) {
        active_search_spaces.push_back(&ue_cfg->search_space(ss.get_id()));
      }
    }
    return active_search_spaces;
  }

  auto filter_ss = [this, pdcch_slot, required_dci_rnti_type](const search_space_info& ss) {
    // See TS 38.213, A UE monitors PDCCH candidates in one or more of the following search spaces sets:
    // - a Type3-PDCCH CSS set configured by SearchSpace in PDCCH-Config with searchSpaceType = common for DCI formats
    //   with CRC scrambled by INT-RNTI, SFI-RNTI, TPC-PUSCH-RNTI, TPC-PUCCH-RNTI, or TPC-SRS-RNTI and, only for the
    //   primary cell, C-RNTI, MCS-C-RNTI, or CS-RNTI(s).
    // - a USS set configured by SearchSpace in PDCCH-Config with searchSpaceType = ue-Specific for DCI formats
    //   with CRC scrambled by C-RNTI, MCS-C-RNTI, SP-CSI-RNTI, or CS-RNTI(s).
    //
    // As per TS 38.213, the UE monitors PDCCH candidates for DCI format 0_0 and DCI format 1_0 with CRC scrambled by
    // the C-RNTI, the MCS-C-RNTI, or the CS-RNTI in the one or more search space sets in a slot where the UE monitors
    // PDCCH candidates for at least a DCI format 0_0 or a DCI format 1_0 with CRC scrambled by SI-RNTI, RA-RNTI or
    // P-RNTI.
    if (ss.cfg->is_common_search_space()) {
      const auto& pdcch_config_ss_lst = cfg().bwp(active_bwp_id()).dl_ded.value()->pdcch_cfg->search_spaces;
      const bool  is_type3_css        = std::find_if(pdcch_config_ss_lst.begin(),
                                             pdcch_config_ss_lst.end(),
                                             [&ss](const search_space_configuration& ss_cfg) {
                                               return ss.cfg->get_id() == ss_cfg.get_id();
                                             }) != pdcch_config_ss_lst.end();

      const bool is_ss_for_ra =
          ss.cfg->get_id() == cfg().cell_cfg_common.dl_cfg_common.init_dl_bwp.pdcch_common.ra_search_space_id;
      const bool is_ss_for_paging =
          not cfg().cell_cfg_common.dl_cfg_common.init_dl_bwp.pdcch_common.paging_search_space_id.has_value() or
          ss.cfg->get_id() ==
              cfg().cell_cfg_common.dl_cfg_common.init_dl_bwp.pdcch_common.paging_search_space_id.value();

      // [Implementation-defined] We exclude SearchSpace#0 to avoid the complexity of computing the SearchSpace#0 PDCCH
      // candidates monitoring occasions associated with a SS/PBCH block as mentioned in TS 38.213, clause 10.1.
      if (ss.cfg->get_id() == to_search_space_id(0) or
          (not is_ss_for_ra and not is_ss_for_paging and not is_type3_css)) {
        return false;
      }
    }

    if (not pdcch_helper::is_pdcch_monitoring_active(pdcch_slot, *ss.cfg)) {
      return false;
    }

    if (ss.get_pdcch_candidates(get_aggregation_level(link_adaptation_controller().get_effective_cqi(), ss, false),
                                pdcch_slot)
            .empty()) {
      return false;
    }
    return (not required_dci_rnti_type.has_value() or
            *required_dci_rnti_type == (ss.get_ul_dci_format() == dci_ul_format::f0_0
                                            ? dci_ul_rnti_config_type::c_rnti_f0_0
                                            : dci_ul_rnti_config_type::c_rnti_f0_1));
  };
  return get_prioritized_search_spaces(*this, filter_ss, false);
}

aggregation_level ue_cell::get_aggregation_level(float cqi, const search_space_info& ss_info, bool is_dl) const
{
  cqi_table_t cqi_table = cqi_table_t::table1;
  unsigned    dci_size;

  if (ss_info.cfg->is_common_search_space()) {
    if (is_dl) {
      dci_size = ss_info.dci_sz.format1_0_common_size.total.value();
    } else {
      dci_size = ss_info.dci_sz.format0_0_common_size.total.value();
    }
  } else {
    if (is_dl) {
      dci_size = ss_info.get_dl_dci_format() == dci_dl_format::f1_1 ? ss_info.dci_sz.format1_1_ue_size->total.value()
                                                                    : ss_info.dci_sz.format1_0_ue_size->total.value();
    } else {
      dci_size = ss_info.get_ul_dci_format() == dci_ul_format::f0_1 ? ss_info.dci_sz.format0_1_ue_size->total.value()
                                                                    : ss_info.dci_sz.format0_0_ue_size->total.value();
    }
  }

  if (not ss_info.cfg->is_common_search_space() and cfg().csi_meas_cfg() != nullptr) {
    // NOTE: It is assumed there is atleast one CSI report configured for UE.
    cqi_table = cfg().csi_meas_cfg()->csi_report_cfg_list.back().cqi_table.value();
  }

  // Apply the configured offset to the CQI and make sure it is in the allowed range.
  static constexpr float MAX_CQI_VALUE = 15.0f, MIN_CQI_VALUE = 0.0f;
  cqi = std::max(std::min(MAX_CQI_VALUE, cqi + expert_cfg.pdcch_al_cqi_offset), MIN_CQI_VALUE);
  return map_cqi_to_aggregation_level(cqi, cqi_table, ss_info.cfg->get_nof_candidates(), dci_size);
}

void ue_cell::apply_link_adaptation_procedures(const csi_report_data& csi_report)
{
  // Early return if no decrease in CQI and RI.
  const bool cqi_decreased = csi_report.first_tb_wideband_cqi.has_value() and
                             csi_report.first_tb_wideband_cqi.value() < components.channel_state->get_wideband_cqi();
  const bool ri_decreased =
      csi_report.ri.has_value() and csi_report.ri.value() < components.channel_state->get_nof_dl_layers();
  if (not cqi_decreased and not ri_decreased) {
    return;
  }

  const csi_report_wideband_cqi_type wideband_cqi = csi_report.first_tb_wideband_cqi.has_value()
                                                        ? csi_report.first_tb_wideband_cqi.value()
                                                        : components.channel_state->get_wideband_cqi();
  const unsigned                     recommended_dl_layers =
      csi_report.ri.has_value() and csi_report.ri.value() <= ue_cfg->get_nof_dl_ports()
                              ? csi_report.ri->value()
                              : components.channel_state->get_nof_dl_layers();

  // Link adaptation for HARQs.
  // [Implementation-defined] If the drop in RI or CQI when compared to the RI or CQI at the time of new HARQ
  // transmission is above threshold then HARQ re-transmissions are cancelled.
  for (unsigned hid = 0; hid != harqs.nof_dl_harqs(); ++hid) {
    std::optional<dl_harq_process_handle> h_dl = harqs.dl_harq(to_harq_id(hid));
    if (not h_dl.has_value()) {
      continue;
    }
    const bool is_ri_diff_above_threshold =
        expert_cfg.dl_harq_la_ri_drop_threshold != 0 and
        recommended_dl_layers + expert_cfg.dl_harq_la_ri_drop_threshold <= h_dl->get_grant_params().nof_layers;
    const bool is_cqi_diff_above_threshold =
        expert_cfg.dl_harq_la_cqi_drop_threshold != 0 and
        wideband_cqi.value() + expert_cfg.dl_harq_la_cqi_drop_threshold <= h_dl->get_grant_params().cqi.value();
    if (is_ri_diff_above_threshold or is_cqi_diff_above_threshold) {
      h_dl->cancel_retxs();
    }
  }
}

double ue_cell::get_estimated_dl_rate(const pdsch_config_params& pdsch_cfg, sch_mcs_index mcs, unsigned nof_prbs) const
{
  const unsigned      dmrs_prbs   = calculate_nof_dmrs_per_rb(pdsch_cfg.dmrs);
  sch_mcs_description mcs_info    = pdsch_mcs_get_config(pdsch_cfg.mcs_table, mcs);
  unsigned            nof_symbols = pdsch_cfg.symbols.length();

  units::bytes tbs_bytes =
      tbs_calculator_calculate(tbs_calculator_configuration{.nof_symb_sh      = nof_symbols,
                                                            .nof_dmrs_prb     = dmrs_prbs,
                                                            .nof_oh_prb       = pdsch_cfg.nof_oh_prb,
                                                            .mcs_descr        = mcs_info,
                                                            .nof_layers       = pdsch_cfg.nof_layers,
                                                            .tb_scaling_field = pdsch_cfg.tb_scaling_field,
                                                            .n_prb            = nof_prbs});

  // Return the estimated throughput, considering that the number of bytes is for a slot.
  return tbs_bytes.value();
}

double ue_cell::get_estimated_ul_rate(const pusch_config_params& pusch_cfg, sch_mcs_index mcs, unsigned nof_prbs) const
{
  const unsigned      dmrs_prbs = calculate_nof_dmrs_per_rb(pusch_cfg.dmrs);
  sch_mcs_description mcs_info =
      pusch_mcs_get_config(pusch_cfg.mcs_table, mcs, pusch_cfg.use_transform_precoder, pusch_cfg.tp_pi2bpsk_present);
  unsigned nof_symbols = pusch_cfg.symbols.length();

  units::bytes tbs_bytes =
      tbs_calculator_calculate(tbs_calculator_configuration{.nof_symb_sh      = nof_symbols,
                                                            .nof_dmrs_prb     = dmrs_prbs,
                                                            .nof_oh_prb       = pusch_cfg.nof_oh_prb,
                                                            .mcs_descr        = mcs_info,
                                                            .nof_layers       = pusch_cfg.nof_layers,
                                                            .tb_scaling_field = pusch_cfg.tb_scaling_field,
                                                            .n_prb            = nof_prbs});

  // Return the estimated throughput, considering that the number of bytes is for a slot.
  return tbs_bytes.value();
}

void ue_cell::set_conres_state(bool state)
{
  if (pcell_state->conres_complete == state) {
    return;
  }
  pcell_state->conres_complete = state;
  if (state) {
    pcell_state->msg3_rx_slot = slot_point{};
    logger.debug("ue={} rnti={}: ConRes procedure completed", fmt::underlying(ue_index), rnti());
  } else {
    logger.debug("ue={} rnti={}: ConRes procedure started", fmt::underlying(ue_index), rnti());
  }
}
