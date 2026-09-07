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

#pragma once

#include <cstdint>
#include <string>

namespace ocudu {

namespace slice_ml_dataset {

struct ue_sample {
  uint64_t period_idx;
  uint32_t slot;
  uint32_t period_ms;

  uint16_t rnti;
  uint16_t ue_index;

  uint8_t  sst;
  uint32_t sd;

  double dl_brate_kbps;
  double ul_brate_kbps;

  uint32_t dl_nof_ok;
  uint32_t dl_nof_nok;
  uint32_t ul_nof_ok;
  uint32_t ul_nof_nok;

  uint32_t tot_pdsch_prbs_used;
  uint32_t tot_pusch_prbs_used;

  uint8_t dl_mcs;
  uint8_t ul_mcs;
  bool    has_mean_cqi;
  float   mean_cqi;
  bool    has_dl_ri;
  float   dl_ri;
  bool    has_ul_ri;
  float   ul_ri;
  float   pusch_snr_db;
  float   pusch_rsrp_db;
  float   pucch_snr_db;

  uint32_t bsr;
  uint32_t dl_bs;
  uint32_t sr_count;

  bool  has_avg_sr_to_pusch_delay_ms;
  float avg_sr_to_pusch_delay_ms;
  bool  has_max_sr_to_pusch_delay_ms;
  float max_sr_to_pusch_delay_ms;
  bool  has_avg_crc_delay_ms;
  float avg_crc_delay_ms;
  bool  has_max_crc_delay_ms;
  float max_crc_delay_ms;
  bool  has_avg_pusch_harq_delay_ms;
  float avg_pusch_harq_delay_ms;
  bool  has_max_pusch_harq_delay_ms;
  float max_pusch_harq_delay_ms;
  bool  has_avg_pucch_harq_delay_ms;
  float avg_pucch_harq_delay_ms;
  bool  has_max_pucch_harq_delay_ms;
  float max_pucch_harq_delay_ms;
  bool  has_avg_ce_delay_ms;
  float avg_ce_delay_ms;
  bool  has_max_ce_delay_ms;
  float max_ce_delay_ms;

  bool  has_last_phr;
  int   last_phr;
  bool  has_last_dl_olla;
  float last_dl_olla;
  bool  has_last_ul_olla;
  float last_ul_olla;
};

struct slice_sample {
  uint64_t period_idx;
  uint32_t slot;
  uint32_t period_ms;

  uint32_t cell_nof_prbs;
  uint16_t pci;

  uint8_t  sst;
  uint32_t sd;

  uint32_t nof_ues;

  uint32_t min_prbs;
  uint32_t max_prbs;
  uint32_t ded_prbs;
  uint32_t min_prbs_ul;
  uint32_t max_prbs_ul;
  uint32_t ded_prbs_ul;
  float min_prb_ratio;
  float max_prb_ratio;
  float min_prb_ratio_ul;
  float max_prb_ratio_ul;

  float avg_dl_rbs_per_slot;
  float avg_ul_rbs_per_slot;
  float dl_prb_share;
  float ul_prb_share;

  double dl_brate_kbps_sum;
  double ul_brate_kbps_sum;
  uint64_t dl_bs_sum;
  uint64_t bsr_sum;

  uint64_t dl_nof_ok_sum;
  uint64_t dl_nof_nok_sum;
  uint64_t ul_nof_ok_sum;
  uint64_t ul_nof_nok_sum;

  bool  has_mean_cqi;
  float mean_cqi;
  bool  has_mean_dl_mcs;
  float mean_dl_mcs;
  bool  has_mean_ul_mcs;
  float mean_ul_mcs;
  bool  has_mean_pusch_snr_db;
  float mean_pusch_snr_db;

  bool  has_mean_sr_to_pusch_delay_ms;
  float mean_sr_to_pusch_delay_ms;
  bool  has_max_sr_to_pusch_delay_ms;
  float max_sr_to_pusch_delay_ms;

  float ssr_embb;
  float ssr_urllc;
  float target_dl_rate_kbps;
  float delay_budget_ms;

  bool     has_power_stats;
  double   power_committed_w;
  double   power_remaining_w;
  uint64_t power_pairs_rejected_unit_taken;
  uint64_t power_pairs_rejected_infeasible;
  uint64_t power_pairs_rejected_no_demand;
  uint64_t power_decode_steps_taken;
  uint64_t power_decode_steps_total;
};

void configure(bool               enabled,
               const std::string& output_dir,
               const std::string& scenario,
               float              target_dl_rate_kbps,
               float              delay_budget_ms);

bool is_ue_enabled();

bool is_slicemanager_enabled();

void log_ue_sample(const ue_sample& s);

void log_slice_sample(const slice_sample& s);

uint64_t next_period_index();

float configured_target_dl_rate_kbps();
float configured_delay_budget_ms();

}
}
