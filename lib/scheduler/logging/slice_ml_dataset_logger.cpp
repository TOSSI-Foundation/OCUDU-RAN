// Copyright 2025-2026 coRAN LABS Private Limited
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#include "slice_ml_dataset_logger.h"
#include "fmt/format.h"
#include <atomic>
#include <cstdio>
#include <ctime>
#include <mutex>
#include <string>
#include <sys/stat.h>
#include <sys/types.h>

static void mkdir_p(const std::string& path)
{
  for (std::size_t pos = 1; pos <= path.size(); ++pos) {
    if (pos == path.size() || path[pos] == '/') {
      ::mkdir(path.substr(0, pos).c_str(), 0775);
    }
  }
}

using namespace ocudu;
using namespace ocudu::slice_ml_dataset;

namespace {

std::string opt_cell(bool has, float v, int precision = 3)
{
  if (not has) {
    return {};
  }
  return fmt::format("{:.{}f}", v, precision);
}

std::string opt_cell_int(bool has, int v)
{
  if (not has) {
    return {};
  }
  return fmt::format("{}", v);
}

std::string timestamp_now_str()
{
  std::time_t t   = std::time(nullptr);
  std::tm     tmv = {};
  ::localtime_r(&t, &tmv);
  char buf[32];
  std::strftime(buf, sizeof(buf), "%Y%m%d_%H%M%S", &tmv);
  return std::string(buf);
}

class ue_dataset_writer
{
public:
  static ue_dataset_writer& instance()
  {
    static ue_dataset_writer inst;
    return inst;
  }

  void configure(bool en, const std::string& output_dir, const std::string& scenario_tag)
  {
    if (configured) {
      return;
    }
    configured = true;
    if (!en) {
      return;
    }
    if (!scenario_tag.empty()) {
      scenario = scenario_tag;
    }
    std::string dir = output_dir.empty() ? std::string("ml/datasets/slice_datasets") : output_dir;
    mkdir_p(dir);
    std::string out  = fmt::format("{}/slice_ml_ue_{}.csv", dir, timestamp_now_str());
    const char* path = out.c_str();
    file             = std::fopen(path, "w");
    if (file == nullptr) {
      std::fprintf(stderr, "[slice_ml_dataset] Failed to open '%s' for writing. Per-UE logging disabled.\n", path);
      return;
    }

    std::fputs("period_idx,slot,period_ms,"
               "rnti,ue_index,sst,sd,"
               "dl_brate_kbps,ul_brate_kbps,"
               "dl_nof_ok,dl_nof_nok,dl_bler_pct,ul_nof_ok,ul_nof_nok,ul_bler_pct,"
               "tot_pdsch_prbs_used,tot_pusch_prbs_used,"
               "dl_mcs,ul_mcs,mean_cqi,dl_ri,ul_ri,pusch_snr_db,pusch_rsrp_db,pucch_snr_db,"
               "bsr,dl_bs,sr_count,"
               "avg_sr_to_pusch_delay_ms,max_sr_to_pusch_delay_ms,"
               "avg_crc_delay_ms,max_crc_delay_ms,"
               "avg_pusch_harq_delay_ms,max_pusch_harq_delay_ms,"
               "avg_pucch_harq_delay_ms,max_pucch_harq_delay_ms,"
               "avg_ce_delay_ms,max_ce_delay_ms,"
               "last_phr,last_dl_olla,last_ul_olla,"
               "scenario\n",
               file);
    std::fflush(file);
    std::fprintf(stderr, "[slice_ml_dataset] Per-UE logging enabled -> %s (scenario=%s)\n", path, scenario.c_str());
  }

  bool enabled() const { return file != nullptr; }

  void write(const ue_sample& s)
  {
    if (file == nullptr) {
      return;
    }

    const uint64_t dl_tot = static_cast<uint64_t>(s.dl_nof_ok) + s.dl_nof_nok;
    const uint64_t ul_tot = static_cast<uint64_t>(s.ul_nof_ok) + s.ul_nof_nok;
    std::string    dl_bler =
        dl_tot > 0 ? fmt::format("{:.4f}", 100.0 * static_cast<double>(s.dl_nof_nok) / static_cast<double>(dl_tot))
                   : std::string();
    std::string ul_bler =
        ul_tot > 0 ? fmt::format("{:.4f}", 100.0 * static_cast<double>(s.ul_nof_nok) / static_cast<double>(ul_tot))
                   : std::string();

    std::string line = fmt::format("{},{},{},"
                                   "{},{},{},{},"
                                   "{:.3f},{:.3f},"
                                   "{},{},{},{},{},{},"
                                   "{},{},"
                                   "{},{},{},{},{},{:.2f},{:.2f},{:.2f},"
                                   "{},{},{},"
                                   "{},{},"
                                   "{},{},"
                                   "{},{},"
                                   "{},{},"
                                   "{},{},"
                                   "{},{},{},"
                                   "{}\n",
                                   s.period_idx, s.slot, s.period_ms,
                                   s.rnti, s.ue_index, static_cast<unsigned>(s.sst), s.sd,
                                   s.dl_brate_kbps, s.ul_brate_kbps,
                                   s.dl_nof_ok, s.dl_nof_nok, dl_bler, s.ul_nof_ok, s.ul_nof_nok, ul_bler,
                                   s.tot_pdsch_prbs_used, s.tot_pusch_prbs_used,
                                   static_cast<unsigned>(s.dl_mcs), static_cast<unsigned>(s.ul_mcs),
                                   opt_cell(s.has_mean_cqi, s.mean_cqi),
                                   opt_cell(s.has_dl_ri, s.dl_ri),
                                   opt_cell(s.has_ul_ri, s.ul_ri),
                                   s.pusch_snr_db, s.pusch_rsrp_db, s.pucch_snr_db,
                                   s.bsr, s.dl_bs, s.sr_count,
                                   opt_cell(s.has_avg_sr_to_pusch_delay_ms, s.avg_sr_to_pusch_delay_ms),
                                   opt_cell(s.has_max_sr_to_pusch_delay_ms, s.max_sr_to_pusch_delay_ms),
                                   opt_cell(s.has_avg_crc_delay_ms, s.avg_crc_delay_ms),
                                   opt_cell(s.has_max_crc_delay_ms, s.max_crc_delay_ms),
                                   opt_cell(s.has_avg_pusch_harq_delay_ms, s.avg_pusch_harq_delay_ms),
                                   opt_cell(s.has_max_pusch_harq_delay_ms, s.max_pusch_harq_delay_ms),
                                   opt_cell(s.has_avg_pucch_harq_delay_ms, s.avg_pucch_harq_delay_ms),
                                   opt_cell(s.has_max_pucch_harq_delay_ms, s.max_pucch_harq_delay_ms),
                                   opt_cell(s.has_avg_ce_delay_ms, s.avg_ce_delay_ms),
                                   opt_cell(s.has_max_ce_delay_ms, s.max_ce_delay_ms),
                                   opt_cell_int(s.has_last_phr, s.last_phr),
                                   opt_cell(s.has_last_dl_olla, s.last_dl_olla),
                                   opt_cell(s.has_last_ul_olla, s.last_ul_olla),
                                   scenario);

    std::lock_guard<std::mutex> guard(mtx);
    std::fputs(line.c_str(), file);
    if ((++rows_since_flush) >= flush_period) {
      std::fflush(file);
      rows_since_flush = 0;
    }
  }

private:
  ue_dataset_writer() = default;

  ~ue_dataset_writer()
  {
    if (file != nullptr) {
      std::fflush(file);
      std::fclose(file);
    }
  }

  std::FILE*                file             = nullptr;
  std::mutex                mtx;
  unsigned                  rows_since_flush = 0;
  std::string               scenario         = "default";
  bool                      configured       = false;
  static constexpr unsigned flush_period     = 64;
};

class slicemanager_dataset_writer
{
public:
  static slicemanager_dataset_writer& instance()
  {
    static slicemanager_dataset_writer inst;
    return inst;
  }

  void configure(bool               en,
                 const std::string& output_dir,
                 const std::string& scenario_tag,
                 float              target_dl_rate_kbps_,
                 float              delay_budget_ms_)
  {
    if (configured) {
      return;
    }
    configured          = true;
    target_dl_rate_kbps = target_dl_rate_kbps_;
    delay_budget_ms     = delay_budget_ms_;
    if (!en) {
      return;
    }
    if (!scenario_tag.empty()) {
      scenario = scenario_tag;
    }
    std::string dir = output_dir.empty() ? std::string("ml/datasets/slice_datasets") : output_dir;
    mkdir_p(dir);
    std::string out  = fmt::format("{}/slice_ml_slicemanager_{}.csv", dir, timestamp_now_str());
    const char* path = out.c_str();
    file             = std::fopen(path, "w");
    if (file == nullptr) {
      std::fprintf(stderr, "[slice_ml_dataset] Failed to open '%s' for writing. SliceManager logging disabled.\n", path);
      return;
    }

    std::fputs("period_idx,slot,period_ms,cell_nof_prbs,pci,"
               "sst,sd,nof_ues,"
               "min_prbs,max_prbs,ded_prbs,min_prbs_ul,max_prbs_ul,ded_prbs_ul,"
               "min_prb_ratio,max_prb_ratio,min_prb_ratio_ul,max_prb_ratio_ul,"
               "avg_dl_rbs_per_slot,avg_ul_rbs_per_slot,dl_prb_share,ul_prb_share,"
               "dl_brate_kbps_sum,ul_brate_kbps_sum,dl_bs_sum,bsr_sum,"
               "dl_nof_ok_sum,dl_nof_nok_sum,dl_bler_pct,ul_nof_ok_sum,ul_nof_nok_sum,ul_bler_pct,"
               "mean_cqi,mean_dl_mcs,mean_ul_mcs,mean_pusch_snr_db,"
               "mean_sr_to_pusch_delay_ms,max_sr_to_pusch_delay_ms,"
               "ssr_embb,ssr_urllc,target_dl_rate_kbps,delay_budget_ms,"
               "power_committed_w,power_remaining_w,"
               "power_pairs_rejected_unit_taken,power_pairs_rejected_infeasible,power_pairs_rejected_no_demand,"
               "power_decode_steps_taken,power_decode_steps_total,"
               "scenario\n",
               file);
    std::fflush(file);
    std::fprintf(stderr,
                 "[slice_ml_dataset] SliceManager logging enabled -> %s (scenario=%s, target_dl_rate=%.1f kbps, "
                 "delay_budget=%.1f ms)\n",
                 path,
                 scenario.c_str(),
                 target_dl_rate_kbps,
                 delay_budget_ms);
  }

  bool enabled() const { return file != nullptr; }

  float target_rate() const { return target_dl_rate_kbps; }
  float budget_ms() const { return delay_budget_ms; }

  void write(const slice_sample& s)
  {
    if (file == nullptr) {
      return;
    }

    const uint64_t dl_tot = s.dl_nof_ok_sum + s.dl_nof_nok_sum;
    const uint64_t ul_tot = s.ul_nof_ok_sum + s.ul_nof_nok_sum;
    std::string    dl_bler =
        dl_tot > 0 ? fmt::format("{:.4f}", 100.0 * static_cast<double>(s.dl_nof_nok_sum) / static_cast<double>(dl_tot))
                   : std::string();
    std::string ul_bler =
        ul_tot > 0 ? fmt::format("{:.4f}", 100.0 * static_cast<double>(s.ul_nof_nok_sum) / static_cast<double>(ul_tot))
                   : std::string();

    std::string ssr_embb_cell  = s.ssr_embb < 0.0f ? std::string() : fmt::format("{:.4f}", s.ssr_embb);
    std::string ssr_urllc_cell = s.ssr_urllc < 0.0f ? std::string() : fmt::format("{:.4f}", s.ssr_urllc);

    std::string power_committed_cell = s.has_power_stats ? fmt::format("{:.6f}", s.power_committed_w) : std::string();
    std::string power_remaining_cell = s.has_power_stats ? fmt::format("{:.6f}", s.power_remaining_w) : std::string();
    std::string power_rej_unit_cell =
        s.has_power_stats ? fmt::format("{}", s.power_pairs_rejected_unit_taken) : std::string();
    std::string power_rej_infeasible_cell =
        s.has_power_stats ? fmt::format("{}", s.power_pairs_rejected_infeasible) : std::string();
    std::string power_rej_no_demand_cell =
        s.has_power_stats ? fmt::format("{}", s.power_pairs_rejected_no_demand) : std::string();
    std::string power_steps_taken_cell =
        s.has_power_stats ? fmt::format("{}", s.power_decode_steps_taken) : std::string();
    std::string power_steps_total_cell =
        s.has_power_stats ? fmt::format("{}", s.power_decode_steps_total) : std::string();

    std::string line = fmt::format("{},{},{},{},{},"
                                   "{},{},{},"
                                   "{},{},{},{},{},{},"
                                   "{:.4f},{:.4f},{:.4f},{:.4f},"
                                   "{:.4f},{:.4f},{:.6f},{:.6f},"
                                   "{:.3f},{:.3f},{},{},"
                                   "{},{},{},{},{},{},"
                                   "{},{},{},{},"
                                   "{},{},"
                                   "{},{},{:.3f},{:.3f},"
                                   "{},{},"
                                   "{},{},{},"
                                   "{},{},"
                                   "{}\n",
                                   s.period_idx, s.slot, s.period_ms, s.cell_nof_prbs, s.pci,
                                   static_cast<unsigned>(s.sst), s.sd, s.nof_ues,
                                   s.min_prbs, s.max_prbs, s.ded_prbs, s.min_prbs_ul, s.max_prbs_ul, s.ded_prbs_ul,
                                   s.min_prb_ratio, s.max_prb_ratio, s.min_prb_ratio_ul, s.max_prb_ratio_ul,
                                   s.avg_dl_rbs_per_slot, s.avg_ul_rbs_per_slot, s.dl_prb_share, s.ul_prb_share,
                                   s.dl_brate_kbps_sum, s.ul_brate_kbps_sum, s.dl_bs_sum, s.bsr_sum,
                                   s.dl_nof_ok_sum, s.dl_nof_nok_sum, dl_bler,
                                   s.ul_nof_ok_sum, s.ul_nof_nok_sum, ul_bler,
                                   opt_cell(s.has_mean_cqi, s.mean_cqi),
                                   opt_cell(s.has_mean_dl_mcs, s.mean_dl_mcs),
                                   opt_cell(s.has_mean_ul_mcs, s.mean_ul_mcs),
                                   opt_cell(s.has_mean_pusch_snr_db, s.mean_pusch_snr_db, 2),
                                   opt_cell(s.has_mean_sr_to_pusch_delay_ms, s.mean_sr_to_pusch_delay_ms),
                                   opt_cell(s.has_max_sr_to_pusch_delay_ms, s.max_sr_to_pusch_delay_ms),
                                   ssr_embb_cell, ssr_urllc_cell, s.target_dl_rate_kbps, s.delay_budget_ms,
                                   power_committed_cell, power_remaining_cell,
                                   power_rej_unit_cell, power_rej_infeasible_cell, power_rej_no_demand_cell,
                                   power_steps_taken_cell, power_steps_total_cell,
                                   scenario);

    std::lock_guard<std::mutex> guard(mtx);
    std::fputs(line.c_str(), file);
    if ((++rows_since_flush) >= flush_period) {
      std::fflush(file);
      rows_since_flush = 0;
    }
  }

private:
  slicemanager_dataset_writer() = default;

  ~slicemanager_dataset_writer()
  {
    if (file != nullptr) {
      std::fflush(file);
      std::fclose(file);
    }
  }

  std::FILE*                file             = nullptr;
  std::mutex                mtx;
  unsigned                  rows_since_flush = 0;
  std::string               scenario         = "default";
  bool                      configured       = false;
  float                     target_dl_rate_kbps = 0.0f;
  float                     delay_budget_ms     = 0.0f;
  static constexpr unsigned flush_period        = 16;
};

std::atomic<uint64_t> period_counter{0};

}

void ocudu::slice_ml_dataset::configure(bool               enabled,
                                        const std::string& output_dir,
                                        const std::string& scenario,
                                        float              target_dl_rate_kbps,
                                        float              delay_budget_ms)
{
  ue_dataset_writer::instance().configure(enabled, output_dir, scenario);
  slicemanager_dataset_writer::instance().configure(enabled, output_dir, scenario, target_dl_rate_kbps, delay_budget_ms);
}

bool ocudu::slice_ml_dataset::is_ue_enabled()
{
  return ue_dataset_writer::instance().enabled();
}

bool ocudu::slice_ml_dataset::is_slicemanager_enabled()
{
  return slicemanager_dataset_writer::instance().enabled();
}

void ocudu::slice_ml_dataset::log_ue_sample(const ue_sample& s)
{
  ue_dataset_writer::instance().write(s);
}

void ocudu::slice_ml_dataset::log_slice_sample(const slice_sample& s)
{
  slicemanager_dataset_writer::instance().write(s);
}

uint64_t ocudu::slice_ml_dataset::next_period_index()
{
  return period_counter.fetch_add(1, std::memory_order_relaxed);
}

float ocudu::slice_ml_dataset::configured_target_dl_rate_kbps()
{
  return slicemanager_dataset_writer::instance().target_rate();
}

float ocudu::slice_ml_dataset::configured_delay_budget_ms()
{
  return slicemanager_dataset_writer::instance().budget_ms();
}
