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

#include "scheduler_time_qos.h"
#include "../support/attention_scheduler_predictor.h"
#include <cstdint>

namespace ocudu {

class scheduler_attention_ml final : public scheduler_policy
{
public:
  scheduler_attention_ml(const attention_ml_scheduler_config& cfg_,
                         const cell_configuration&            cell_cfg_,
                         unsigned                             slice_dedicated_rbs);

  void add_ue(du_ue_index_t ue_index) override { fallback.add_ue(ue_index); }

  void rem_ue(du_ue_index_t ue_index) override { fallback.rem_ue(ue_index); }

  void compute_ue_dl_priorities(slot_point               pdcch_slot,
                                slot_point               pdsch_slot,
                                span<ue_newtx_candidate> ue_candidates) override;

  void compute_ue_ul_priorities(slot_point               pdcch_slot,
                                slot_point               pusch_slot,
                                span<ue_newtx_candidate> ue_candidates) override
  {
    fallback.compute_ue_ul_priorities(pdcch_slot, pusch_slot, ue_candidates);
  }

  void on_slice_reconfiguration(unsigned slice_dedicated_rbs) override;

  void save_dl_newtx_grants(span<const dl_msg_alloc> dl_grants) override
  {
    fallback.save_dl_newtx_grants(dl_grants);
  }

  void save_ul_newtx_grants(span<const ul_sched_info> ul_grants) override
  {
    fallback.save_ul_newtx_grants(ul_grants);
  }

  scheduler_policy_power_stats consume_power_stats() override;

private:
  static constexpr unsigned max_pairs = 2048;
  static constexpr unsigned max_trained_units = 48;

  bool run_rollout(span<ue_newtx_candidate> ue_candidates);

  void build_features(span<const ue_newtx_candidate> ue_candidates, unsigned n_ues);

  const attention_ml_scheduler_config cfg;
  const cell_configuration&           cell_cfg;
  scheduler_time_qos                  fallback;

  std::shared_ptr<attn_sched::model> mdl;
  bool     enabled             = false;
  unsigned consecutive_misses  = 0;
  unsigned deadline_us         = 0;
  unsigned n_units             = 0;
  unsigned n_dedicated_units   = 0;
  int      vrb_origin_offset   = 0;
  double   unit_blocklength    = 0.0;
  double   unit_bw_hz          = 0.0;

  static constexpr unsigned stats_period    = 4096;
  unsigned                  rollout_count   = 0;
  std::uint64_t             rollout_us_sum  = 0;
  unsigned                  rollout_us_max  = 0;

  unsigned      power_nof_rollouts                = 0;
  double        power_committed_w_sum             = 0.0;
  double        power_remaining_w_sum             = 0.0;
  std::uint64_t power_pairs_rejected_unit_taken    = 0;
  std::uint64_t power_pairs_rejected_infeasible    = 0;
  std::uint64_t power_pairs_rejected_no_demand     = 0;
  std::uint64_t power_decode_steps_taken           = 0;
  std::uint64_t power_decode_steps_total           = 0;

  attn_sched::workspace ws;
  std::vector<float>    phi, delta, h_prev, c_vec;
  std::vector<uint8_t>  mask, unit_taken, ever_selectable;
  std::vector<double>   pair_power, pair_rate;
  std::vector<double>   alloc_count, remaining, user_rate;
  std::vector<vrb_bitmap> ue_units;
};

}
