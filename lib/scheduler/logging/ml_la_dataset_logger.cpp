// SPDX-FileCopyrightText: Copyright (C) 2021-2026 Software Radio Systems Limited
// SPDX-License-Identifier: BSD-3-Clause-Open-MPI
// Portions of this file may implement 3GPP specifications, which may be subject to additional licensing requirements.

#include "ml_la_dataset_logger.h"
#include "fmt/format.h"
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <mutex>
#include <string>
#include <sys/stat.h>
#include <sys/types.h>

using namespace ocudu;
using namespace ocudu::ml_la_dataset;

namespace {

/// Singleton writer for the UL link-adaptation dataset. Opened by configure() from the scheduler config.
class dataset_writer
{
public:
  static dataset_writer& instance()
  {
    static dataset_writer inst;
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
    std::string dir = output_dir.empty() ? std::string("ml/datasets") : output_dir;
    ::mkdir(dir.c_str(), 0775);
    std::string out  = fmt::format("{}/ul_mcs_dataset_{}.csv", dir, timestamp_now());
    const char* path = out.c_str();
    file             = std::fopen(path, "w");
    if (file == nullptr) {
      std::fprintf(stderr, "[ml_la_dataset] Failed to open '%s' for writing. Dataset logging disabled.\n", path);
      return;
    }
    std::fputs("slot,rnti,ue_index,harq_id,nof_retxs,"
               "mcs,mcs_table,tbs_bytes,nof_prbs,nof_symbols,nof_layers,olla_mcs,"
               "wideband_cqi,pusch_snr_db,pusch_avg_sinr_db,ul_snr_offset_db,"
               "ul_sinr_db,ul_rsrp_dbfs,ta_ns,"
               "nof_dl_layers,nof_ul_layers,csi_ri,csi_cri,slots_since_srs,"
               "dl_cqi_offset_db,effective_cqi,ul_olla_enabled,dl_olla_enabled,"
               "ul_bytes_in_harq,nof_empty_ul_harqs,nof_ul_harqs,max_nof_ul_retxs,ndi,"
               "slice_id,dci_cfg_type,"
               "sfn,slot_in_frame,subframe,numerology,"
               "is_fallback,ml_in_control,ml_p_succ,scenario,"
               "crc_success\n",
               file);
    std::fflush(file);
    std::fprintf(stderr, "[ml_la_dataset] UL MCS dataset logging enabled -> %s (scenario=%s)\n", path,
                 scenario.c_str());
  }

  bool enabled() const { return file != nullptr; }

  void write(const ul_sample& s)
  {
    if (file == nullptr) {
      return;
    }

    // Format optional fields with explicit sentinels so the CSV parses cleanly downstream.
    const auto fopt = [](float v) { return std::isnan(v) ? std::string("nan") : fmt::format("{:.2f}", v); };

    const std::string ta = s.ta_ns == INT32_MIN ? std::string("nan") : fmt::format("{}", s.ta_ns);

    std::string line = fmt::format("{},{},{},{},{},"          // keys/context
                                   "{},{},{},{},{},{},{},"     // action
                                   "{},{:.2f},{:.2f},{:.2f},"  // channel state
                                   "{},{},{},"                 // phy measurements at RX
                                   "{},{},{},{},{},"           // spatial / CSI
                                   "{:.2f},{:.2f},{},{},"      // link-adaptation / OLLA
                                   "{},{},{},{},{},"           // HARQ / queue
                                   "{},{},"                    // slicing / dci
                                   "{},{},{},{},"              // timing decomposition
                                   "{},"                       // state flags
                                   "{},{},"                    // ML predictor verdict
                                   "{},"                       // scenario tag
                                   "{}\n",                     // label
                                   s.slot,
                                   s.rnti,
                                   s.ue_index,
                                   s.harq_id,
                                   s.nof_retxs,
                                   s.mcs,
                                   s.mcs_table,
                                   s.tbs_bytes,
                                   s.nof_prbs,
                                   s.nof_symbols,
                                   s.nof_layers,
                                   s.olla_mcs,
                                   s.wideband_cqi,
                                   s.pusch_snr_db,
                                   s.pusch_avg_sinr_db,
                                   s.ul_snr_offset_db,
                                   fopt(s.ul_sinr_db),
                                   fopt(s.ul_rsrp_dbfs),
                                   ta,
                                   s.nof_dl_layers,
                                   s.nof_ul_layers,
                                   s.csi_ri,
                                   s.csi_cri,
                                   s.slots_since_srs,
                                   s.dl_cqi_offset_db,
                                   s.effective_cqi,
                                   s.ul_olla_enabled,
                                   s.dl_olla_enabled,
                                   s.ul_bytes_in_harq,
                                   s.nof_empty_ul_harqs,
                                   s.nof_ul_harqs,
                                   s.max_nof_ul_retxs,
                                   s.ndi,
                                   s.slice_id,
                                   s.dci_cfg_type,
                                   s.sfn,
                                   s.slot_in_frame,
                                   s.subframe,
                                   s.numerology,
                                   s.is_fallback,
                                   s.ml_in_control,
                                   fopt(s.ml_p_succ),
                                   scenario,
                                   s.crc_success);

    std::lock_guard<std::mutex> guard(mtx);
    std::fputs(line.c_str(), file);
    // Flush periodically so partial datasets survive a crash/Ctrl-C, without paying fflush() on every row.
    if ((++rows_since_flush) >= flush_period) {
      std::fflush(file);
      rows_since_flush = 0;
    }
  }

private:
  static std::string timestamp_now()
  {
    std::time_t t   = std::time(nullptr);
    std::tm     tmv {};
    ::localtime_r(&t, &tmv);
    char buf[32];
    std::strftime(buf, sizeof(buf), "%Y%m%d_%H%M%S", &tmv);
    return std::string(buf);
  }

  dataset_writer() = default;

  ~dataset_writer()
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

} // namespace

void ocudu::ml_la_dataset::configure(bool enabled, const std::string& output_dir, const std::string& scenario)
{
  dataset_writer::instance().configure(enabled, output_dir, scenario);
}

bool ocudu::ml_la_dataset::is_enabled()
{
  return dataset_writer::instance().enabled();
}

void ocudu::ml_la_dataset::log_ul_sample(const ul_sample& s)
{
  dataset_writer::instance().write(s);
}
