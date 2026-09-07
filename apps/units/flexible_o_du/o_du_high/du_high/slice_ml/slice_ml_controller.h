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

#include "lib/scheduler/support/slice_ml_predictor.h"
#include "ocudu/du/du_high/du_manager/du_configurator.h"
#include "ocudu/ocudulog/ocudulog.h"
#include "ocudu/ran/rrm.h"
#include "ocudu/scheduler/config/scheduler_expert_config.h"
#include "ocudu/scheduler/scheduler_metrics.h"
#include <array>
#include <optional>
#include <vector>

namespace ocudu {

struct slice_ml_action {
  uint8_t n_embb_twelfths;
  uint8_t n_shared_twelfths;
  uint8_t n_urllc_twelfths;

  unsigned min_ratio_embb;
  unsigned max_ratio_embb;
  unsigned min_ratio_urllc;
  unsigned max_ratio_urllc;
};

inline constexpr std::array<slice_ml_action, 19> SLICE_ML_ACTION_MENU = {{
    {0, 12, 0, 0, 100, 0, 100},
    {6, 0, 6, 50, 50, 50, 50},
    {4, 4, 4, 33, 67, 33, 67},
    {8, 4, 0, 67, 100, 0, 33},
    {8, 0, 4, 67, 67, 33, 33},
    {4, 0, 8, 33, 33, 67, 67},
    {4, 8, 0, 33, 100, 0, 67},
    {0, 8, 4, 0, 67, 33, 100},
    {0, 4, 8, 0, 33, 67, 100},
    {8, 2, 2, 67, 83, 17, 33},
    {2, 8, 2, 17, 83, 17, 83},
    {2, 2, 8, 17, 33, 67, 83},
    {0, 0, 0, 70, 100, 30, 100},
    {0, 0, 0, 50, 100, 50, 100},
    {0, 0, 0, 60, 100, 40, 100},
    {0, 0, 0, 40, 100, 60, 100},
    {0, 0, 0, 80, 100, 20, 100},
    {0, 0, 0, 30, 100, 70, 100},
    {0, 0, 0, 90, 100, 10, 100},
}};

struct slice_ml_reward_weights {
  static constexpr double LAMBDA_SP    = 0.01;
  static constexpr double LAMBDA_EMBB  = 5.0;
  static constexpr double LAMBDA_URLLC = 5.0;
  static constexpr double UPSILON_EMBB  = 0.90;
  static constexpr double UPSILON_URLLC = 0.99;
};

inline double slice_ml_utility(double embb_throughput_mbps, double embb_ssr, double urllc_ssr)
{
  using w                   = slice_ml_reward_weights;
  const double embb_short   = w::UPSILON_EMBB - embb_ssr;
  const double urllc_short  = w::UPSILON_URLLC - urllc_ssr;
  return w::LAMBDA_SP * embb_throughput_mbps - w::LAMBDA_EMBB * (embb_short > 0.0 ? embb_short : 0.0) -
         w::LAMBDA_URLLC * (urllc_short > 0.0 ? urllc_short : 0.0);
}

struct slice_ml_decision {
  bool     ran            = false;
  unsigned selected_idx   = 0;
  unsigned applied_idx    = 0;
  bool     actuated       = false;
  double   selected_prob  = 0.0;
  double   utility        = 0.0;
  const char* blocked_reason = nullptr;
};

struct slice_ml_observation_pair {
  slice_ml::slice_ml_observation obs;
  bool                           embb_seen  = false;
  bool                           urllc_seen = false;
};

inline constexpr uint64_t HEARTBEAT_PERIODS = 60;

class slice_ml_controller
{
public:
  slice_ml_controller(const slice_ml_expert_config& cfg_,
                      plmn_identity                 plmn_,
                      uint8_t                       embb_sst_,
                      uint32_t                      embb_sd_,
                      uint8_t                       urllc_sst_,
                      uint32_t                      urllc_sd_);

  void set_configurator(odu::du_configurator& configurator_) { configurator = &configurator_; }

  slice_ml_decision handle_report(const scheduler_cell_metrics& cell);

private:
  std::optional<slice_ml_observation_pair> build_observation(const scheduler_cell_metrics& cell) const;

  unsigned apply_safety_gates(unsigned proposed_idx, const char*& reason) const;

  bool actuate(unsigned action_idx);

  void verify_last_actuation(const scheduler_cell_metrics& cell);

  slice_ml_expert_config cfg;
  odu::du_configurator* configurator = nullptr;
  plmn_identity         plmn;
  uint8_t               embb_sst;
  uint32_t              embb_sd;
  uint8_t               urllc_sst;
  uint32_t              urllc_sd;

  ocudulog::basic_logger& logger;

  std::vector<double> lstm_h;
  std::vector<double> lstm_c;
  unsigned            model_generation = 0;

  unsigned current_action_idx = 0;
  unsigned pending_action_idx = 0;
  unsigned pending_streak     = 0;
  unsigned periods_since_switch = 0;
  bool     awaiting_verify      = false;
  uint64_t period_counter       = 0;
};

}
