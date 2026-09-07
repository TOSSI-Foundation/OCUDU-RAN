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

#include "slice_ml_controller.h"
#include <cmath>

using namespace ocudu;

slice_ml_controller::slice_ml_controller(const slice_ml_expert_config& cfg_,
                                         plmn_identity                 plmn_,
                                         uint8_t                       embb_sst_,
                                         uint32_t                      embb_sd_,
                                         uint8_t                       urllc_sst_,
                                         uint32_t                      urllc_sd_) :
  cfg(cfg_),
  plmn(plmn_),
  embb_sst(embb_sst_),
  embb_sd(embb_sd_),
  urllc_sst(urllc_sst_),
  urllc_sd(urllc_sd_),
  logger(ocudulog::fetch_basic_logger("DU"))
{
  const unsigned dflt = cfg.default_action_idx < SLICE_ML_ACTION_MENU.size() ? cfg.default_action_idx : 2;
  current_action_idx  = dflt;
  pending_action_idx  = dflt;

  slice_ml::predictor::instance().configure(cfg);

  if (cfg.inference_enabled) {
    const auto& a = SLICE_ML_ACTION_MENU[dflt];
    logger.info("SLICE_ML: controller enabled (apply={}), default action {} -> eMBB min/max {}/{}%, "
                "URLLC min/max {}/{}%",
                cfg.inference_apply ? "yes" : "no (shadow mode)",
                dflt,
                a.min_ratio_embb,
                a.max_ratio_embb,
                a.min_ratio_urllc,
                a.max_ratio_urllc);
  }
}

std::optional<slice_ml_observation_pair>
slice_ml_controller::build_observation(const scheduler_cell_metrics& cell) const
{
  slice_ml_observation_pair out;

  for (const scheduler_slice_metrics& sm : cell.slice_metrics) {
    if (sm.sst == embb_sst) {
      out.obs.embb_throughput_mbps = sm.dl_brate_kbps_sum / 1000.0;
      out.obs.embb_ssr = sm.ssr_embb;
      out.embb_seen    = sm.ssr_embb >= 0.0f;
    } else if (sm.sst == urllc_sst) {
      out.obs.urllc_demand_bytes = static_cast<double>(sm.dl_bs_sum) + static_cast<double>(sm.bsr_sum);
      out.obs.urllc_ssr = sm.ssr_urllc;
      out.urllc_seen    = sm.ssr_urllc >= 0.0f;
    }
  }

  if (!out.embb_seen || !out.urllc_seen) {
    return std::nullopt;
  }
  return out;
}

unsigned slice_ml_controller::apply_safety_gates(unsigned proposed_idx, const char*& reason) const
{
  reason = nullptr;

  if (proposed_idx >= SLICE_ML_ACTION_MENU.size()) {
    reason = "action index out of range";
    return current_action_idx;
  }
  const slice_ml_action& a = SLICE_ML_ACTION_MENU[proposed_idx];

  if (a.min_ratio_urllc < cfg.min_urllc_prb_ratio) {
    reason = "URLLC min ratio below configured floor";
    return current_action_idx;
  }

  if (a.min_ratio_embb + a.min_ratio_urllc > cfg.max_total_min_ratio) {
    reason = "sum of reserved ratios exceeds configured cap";
    return current_action_idx;
  }

  if (proposed_idx != current_action_idx && periods_since_switch < cfg.min_periods_between_switches) {
    reason = "minimum interval between switches not elapsed";
    return current_action_idx;
  }

  return proposed_idx;
}

bool slice_ml_controller::actuate(unsigned action_idx)
{
  if (configurator == nullptr) {
    logger.warning("SLICE_ML: actuation requested before the DU configurator was bound; ignoring");
    return false;
  }
  const slice_ml_action& a = SLICE_ML_ACTION_MENU[action_idx];

  auto make_group = [this](uint8_t sst, uint32_t sd, unsigned min_ratio, unsigned max_ratio) {
    rrm_policy_ratio_group g;
    g.resource_type = rrm_policy_ratio_group::resource_type_t::prb;

    rrm_policy_member member;
    member.plmn_id     = plmn;
    member.s_nssai.sst = slice_service_type{sst};
    if (auto v = slice_differentiator::create(sd); v.has_value()) {
      member.s_nssai.sd = v.value();
    }
    g.policy_members_list.push_back(member);

    g.minimum_ratio = min_ratio;
    g.maximum_ratio = max_ratio;
    return g;
  };

  odu::du_param_config_request req;
  req.cells.emplace_back(
      std::nullopt,
      std::nullopt,
      std::vector<rrm_policy_ratio_group>{make_group(embb_sst, embb_sd, a.min_ratio_embb, a.max_ratio_embb),
                                          make_group(urllc_sst, urllc_sd, a.min_ratio_urllc, a.max_ratio_urllc)});

  const bool ok = configurator->handle_sync_operator_config(req).success;
  if (!ok) {
    logger.warning("SLICE_ML: DU rejected the RRM policy ratio change for action {}", action_idx);
  }
  return ok;
}

void slice_ml_controller::verify_last_actuation(const scheduler_cell_metrics& cell)
{
  if (!awaiting_verify or cell.nof_prbs == 0) {
    return;
  }
  awaiting_verify = false;

  const slice_ml_action& a     = SLICE_ML_ACTION_MENU[current_action_idx];
  const double           scale = 100.0 / static_cast<double>(cell.nof_prbs);
  for (const scheduler_slice_metrics& sm : cell.slice_metrics) {
    unsigned want = 0;
    if (sm.sst == embb_sst) {
      want = a.min_ratio_embb;
    } else if (sm.sst == urllc_sst) {
      want = a.min_ratio_urllc;
    } else {
      continue;
    }
    const double got = sm.min_prbs * scale;
    if (std::abs(got - static_cast<double>(want)) > scale + 0.5) {
      logger.warning("SLICE_ML: actuation of action {} did NOT take effect for sst={}: requested min {}%, "
                     "cell reports {:.1f}%. The reconfiguration was accepted but dropped — check that "
                     "slice_ml.inference embb_sd/urllc_sd match cell_cfg.slicing.",
                     current_action_idx,
                     sm.sst,
                     want,
                     got);
    }
  }
}

slice_ml_decision slice_ml_controller::handle_report(const scheduler_cell_metrics& cell)
{
  slice_ml_decision dec;
  dec.applied_idx = current_action_idx;

  if (!cfg.inference_enabled) {
    return dec;
  }

  ++period_counter;
  ++periods_since_switch;

  verify_last_actuation(cell);

  slice_ml::predictor& pred = slice_ml::predictor::instance();
  pred.maybe_reload();

  std::shared_ptr<const slice_ml::actor_model> m = pred.model();
  if (!m || !m->valid()) {
    dec.blocked_reason = "no valid actor model";
    return dec;
  }

  if (pred.generation() != model_generation || lstm_h.size() != m->d_hidden) {
    model_generation = pred.generation();
    lstm_h.assign(m->d_hidden, 0.0);
    lstm_c.assign(m->d_hidden, 0.0);
  }

  std::optional<slice_ml_observation_pair> obs = build_observation(cell);
  if (!obs.has_value()) {
    dec.blocked_reason = "incomplete observation (a slice was absent or its SSR not computable)";
    return dec;
  }

  dec.utility = slice_ml_utility(obs->obs.embb_throughput_mbps, obs->obs.embb_ssr, obs->obs.urllc_ssr);

  std::vector<double> x;
  x.reserve(m->n_in);
  obs->obs.to_features(x);
  for (unsigned i = 0; i != SLICE_ML_ACTION_MENU.size(); ++i) {
    x.push_back(i == current_action_idx ? 1.0 : 0.0);
  }
  if (x.size() != m->n_in) {
    dec.blocked_reason = "model input width does not match the observation and action menu";
    return dec;
  }

  m->lstm_step(x, lstm_h, lstm_c);
  const std::vector<double> probs = slice_ml::actor_model::softmax(m->logits_from_hidden(lstm_h));

  unsigned best = 0;
  for (unsigned i = 1; i < probs.size(); ++i) {
    if (probs[i] > probs[best]) {
      best = i;
    }
  }
  dec.ran           = true;
  dec.selected_idx  = best;
  dec.selected_prob = probs.empty() ? 0.0 : probs[best];

  if (best == current_action_idx) {
    pending_streak     = 0;
    pending_action_idx = current_action_idx;
  } else if (best == pending_action_idx) {
    ++pending_streak;
  } else {
    pending_action_idx = best;
    pending_streak     = 1;
  }

  const bool streak_met = pending_streak >= cfg.switch_hysteresis_periods;
  if (!streak_met) {
    dec.applied_idx    = current_action_idx;
    dec.blocked_reason = (best != current_action_idx) ? "hysteresis: change not yet confirmed" : nullptr;
    return dec;
  }

  if (period_counter % HEARTBEAT_PERIODS == 1) {
    logger.info("SLICE_ML: period {} obs(tput={:.2f} Mbps, demand={:.0f} B, ssr_e={:.3f}, ssr_u={:.3f}) "
                "U={:.4f} -> action {} (p={:.3f}), in force {}",
                period_counter,
                obs->obs.embb_throughput_mbps,
                obs->obs.urllc_demand_bytes,
                obs->obs.embb_ssr,
                obs->obs.urllc_ssr,
                dec.utility,
                best,
                dec.selected_prob,
                current_action_idx);
  }

  const char* gate_reason = nullptr;
  const unsigned gated    = apply_safety_gates(best, gate_reason);
  dec.blocked_reason      = gate_reason;
  dec.applied_idx         = gated;

  if (gated == current_action_idx) {
    return dec;
  }

  if (!cfg.inference_apply) {
    logger.info("SLICE_ML: [shadow] would switch action {} -> {} (p={:.3f}, U={:.4f})",
                current_action_idx,
                gated,
                dec.selected_prob,
                dec.utility);
    dec.blocked_reason = "shadow mode (inference_apply is false)";
    pending_streak     = 0;
    return dec;
  }

  if (actuate(gated)) {
    const slice_ml_action& a = SLICE_ML_ACTION_MENU[gated];
    logger.info("SLICE_ML: switched action {} -> {} (p={:.3f}, U={:.4f}); eMBB {}/{}%, URLLC {}/{}%",
                current_action_idx,
                gated,
                dec.selected_prob,
                dec.utility,
                a.min_ratio_embb,
                a.max_ratio_embb,
                a.min_ratio_urllc,
                a.max_ratio_urllc);
    current_action_idx   = gated;
    periods_since_switch = 0;
    awaiting_verify      = true;
    dec.actuated         = true;
  } else {
    dec.blocked_reason = "actuation rejected by the DU";
  }
  pending_streak = 0;
  return dec;
}
