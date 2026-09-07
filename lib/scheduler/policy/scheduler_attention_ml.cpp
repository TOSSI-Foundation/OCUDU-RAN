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

#include "scheduler_attention_ml.h"
#include "../slicing/slice_ue_repository.h"
#include "../ue_context/ue_cell.h"
#include "ocudu/ocudulog/ocudulog.h"
#include <algorithm>
#include <chrono>
#include <cmath>

using namespace ocudu;

namespace {

constexpr double NOISE_W     = 1e-14;
constexpr double P_MAX_W     = 1.0;
constexpr double GAMMA_THR   = 3.1622776601683795;
constexpr double QINV_EPS_B  = 4.753424308822899;
constexpr double R_MIN_BPS   = 4e6;

constexpr double blocklength_per_prb(unsigned numerology)
{
  return numerology == 0 ? 24.0 : (numerology == 1 ? 48.0 : 96.0);
}
constexpr double prb_bandwidth_hz(unsigned numerology)
{
  return numerology == 0 ? 180e3 : (numerology == 1 ? 360e3 : 720e3);
}

const double channel_dispersion = 1.0 - 1.0 / ((1.0 + GAMMA_THR) * (1.0 + GAMMA_THR));
const double shannon_per_symbol = std::log2(1.0 + GAMMA_THR);
const double penalty_coef       = std::log2(M_E) * QINV_EPS_B * std::sqrt(channel_dispersion);

double urllc_rate_bits(double n_units, double unit_blocklength)
{
  const double x = unit_blocklength * n_units;
  return std::max(0.0, x * shannon_per_symbol - penalty_coef * std::sqrt(x));
}

double dl_snr_linear(const ue_cell& cc)
{
  static constexpr pdsch_mcs_table ref_mcs_table = pdsch_mcs_table::qam256;

  const auto mcs = cc.link_adaptation_controller().calculate_dl_mcs(ref_mcs_table);
  if (not mcs.has_value()) {
    return 0.0;
  }
  const double se = pdsch_mcs_get_config(ref_mcs_table, mcs.value()).get_spectral_efficiency();
  return std::max(0.0, std::exp2(se) - 1.0);
}

double gain_from_snr(double snr_lin, double power_per_unit)
{
  return snr_lin * NOISE_W / std::max(power_per_unit, 1e-12);
}

}

scheduler_attention_ml::scheduler_attention_ml(const attention_ml_scheduler_config& cfg_,
                                               const cell_configuration&            cell_cfg_,
                                               unsigned                             slice_dedicated_rbs) :
  cfg(cfg_), cell_cfg(cell_cfg_), fallback(cfg_.fallback, cell_cfg_)
{
  ocudulog::basic_logger& logger = ocudulog::fetch_basic_logger("SCHED");

  const unsigned cell_rbs   = cell_cfg.dl_cfg_common.init_dl_bwp.generic_params.crbs.length();
  const unsigned group      = std::max(1u, cfg.prb_group);
  const unsigned numerology = to_numerology_value(cell_cfg.scs_common);

  const bool is_urllc = cfg.role == attention_ml_scheduler_config::role_type::urllc;

  n_units           = cell_rbs / group;
  n_dedicated_units = std::min(n_units, slice_dedicated_rbs / group);
  unit_blocklength  = blocklength_per_prb(numerology) * group;
  unit_bw_hz        = prb_bandwidth_hz(numerology) * group;

  const unsigned slot_us = 1000u >> numerology;
  deadline_us            = cfg.max_inference_us != 0 ? std::min(cfg.max_inference_us, slot_us) : slot_us;

  if (cfg.model_path.empty()) {
    return;
  }
  mdl = attn_sched::load_model_file(cfg.model_path);
  if (mdl == nullptr) {
    logger.warning("attention_ml: could not load '{}'. Falling back to the QoS policy.", cfg.model_path);
    return;
  }

  const char*    want_role = is_urllc ? "urllc" : "embb";
  const unsigned want_feat = is_urllc ? 3u : 2u;
  const unsigned want_ctx  = is_urllc ? 2u : 1u;

  if (mdl->role != want_role) {
    logger.warning("attention_ml: '{}' has role '{}' but the slice is configured as '{}'. Falling back.",
                   cfg.model_path,
                   mdl->role,
                   want_role);
  } else if (mdl->n_feat != want_feat or mdl->n_ctx != want_ctx) {
    logger.warning("attention_ml: '{}' has n_feat={} n_ctx={}, expected {} and {}. Falling back.",
                   cfg.model_path,
                   mdl->n_feat,
                   mdl->n_ctx,
                   want_feat,
                   want_ctx);
  } else if (n_units == 0 or n_units > max_trained_units) {
    logger.warning("attention_ml: {} PRBs at prb_group={} gives {} units, outside the 1..{} the model was trained "
                   "over. Raise prb_group. Falling back.",
                   cell_rbs,
                   group,
                   n_units,
                   max_trained_units);
  } else {
    enabled = true;
    logger.info("attention_ml: '{}' loaded for role '{}': {} units of {} PRBs, {} feats. Deadline {} us.",
                cfg.model_path,
                want_role,
                n_units,
                group,
                mdl->n_feat,
                deadline_us);
  }

  if (not enabled) {
    mdl.reset();
    return;
  }

  const size_t k = std::min<size_t>(static_cast<size_t>(n_units) * MAX_NOF_DU_UES, max_pairs);
  ws.reserve(mdl->d_model, k);
  phi.reserve(k * mdl->n_feat);
  delta.reserve(k);
  pair_power.reserve(k);
  pair_rate.reserve(k);
  mask.reserve(k);
  h_prev.assign(mdl->d_model, 0.f);
  c_vec.assign(mdl->n_ctx, 0.f);
  unit_taken.reserve(n_units);
  alloc_count.reserve(MAX_NOF_DU_UES);
  remaining.reserve(MAX_NOF_DU_UES);
  user_rate.reserve(MAX_NOF_DU_UES);
  ever_selectable.reserve(MAX_NOF_DU_UES);
  ue_units.reserve(MAX_NOF_DU_UES);
}

void scheduler_attention_ml::on_slice_reconfiguration(unsigned slice_dedicated_rbs)
{
  n_dedicated_units = std::min(n_units, slice_dedicated_rbs / std::max(1u, cfg.prb_group));
}

void scheduler_attention_ml::build_features(span<const ue_newtx_candidate> ue_candidates, unsigned n_ues)
{
  const bool   is_urllc = cfg.role == attention_ml_scheduler_config::role_type::urllc;
  const size_t k        = static_cast<size_t>(n_ues) * n_units;
  const double p_unit   = P_MAX_W / static_cast<double>(n_units);

  phi.assign(k * mdl->n_feat, 0.f);
  delta.assign(k, 0.f);
  mask.assign(k, 0);
  pair_power.assign(is_urllc ? k : 0, 0.0);
  pair_rate.assign(is_urllc ? 0 : k, 0.0);
  alloc_count.assign(n_ues, 0.0);
  remaining.assign(n_ues, 0.0);
  user_rate.assign(n_ues, 0.0);
  unit_taken.assign(n_units, 0);
  ever_selectable.assign(n_ues, 0);
  ue_units.assign(n_ues, vrb_bitmap(n_units));

  for (unsigned u = 0; u != n_ues; ++u) {
    const slice_ue& ue     = *ue_candidates[u].ue;
    const double    snr    = dl_snr_linear(ue.get_cc());
    const double    gain   = gain_from_snr(snr, p_unit);
    const float     log_g  = static_cast<float>(std::log10(std::max(gain, 1e-30)));
    const double    demand = static_cast<double>(ue_candidates[u].pending_bytes.value()) * 8.0;

    remaining[u] = demand;

    for (unsigned n = 0; n != n_units; ++n) {
      const size_t pair = static_cast<size_t>(u) * n_units + n;
      const float shared = n >= n_dedicated_units ? 1.f : 0.f;

      if (is_urllc) {
        phi[pair * 3 + 0]  = log_g;
        phi[pair * 3 + 1]  = static_cast<float>(demand / 1e3);
        phi[pair * 3 + 2]  = shared;
        pair_power[pair]   = GAMMA_THR * NOISE_W / std::max(gain, 1e-30);
      } else {
        phi[pair * 2 + 0] = log_g;
        phi[pair * 2 + 1] = shared;
        pair_rate[pair] = unit_bw_hz * std::log2(1.0 + p_unit * gain / NOISE_W);
      }
    }
  }
}

bool scheduler_attention_ml::run_rollout(span<ue_newtx_candidate> ue_candidates)
{
  const unsigned n_ues = ue_candidates.size();
  if (n_ues == 0 or n_units == 0 or static_cast<size_t>(n_ues) * n_units > max_pairs) {
    return false;
  }
  const bool     is_urllc = cfg.role == attention_ml_scheduler_config::role_type::urllc;
  const unsigned k        = n_ues * n_units;

  static constexpr search_space_id ue_ded_ss_id = to_search_space_id(2);
  const search_space_info&         ss           = ue_candidates[0].ue->get_cc().cfg().search_space(ue_ded_ss_id);
  const int                        init_bwp_crb_start =
      static_cast<int>(cell_cfg.dl_cfg_common.init_dl_bwp.generic_params.crbs.start());
  vrb_origin_offset                 = init_bwp_crb_start - static_cast<int>(ss.dl_crb_lims.start());
  const unsigned dedicated_bwp_rbs  = ss.dl_crb_lims.length();

  build_features(ue_candidates, n_ues);

  double remaining_power = P_MAX_W;
  double n_free          = n_units;

  mdl->encode(phi.data(), k, ws);
  mdl->prepare_decode(ws);
  std::fill(h_prev.begin(), h_prev.end(), 0.f);

  unsigned rollout_decode_steps = 0;
  for (unsigned step = 0; step != n_units; ++step) {
    bool any = false;
    for (unsigned u = 0; u != n_ues; ++u) {
      for (unsigned n = 0; n != n_units; ++n) {
        const size_t pair      = static_cast<size_t>(u) * n_units + n;
        const bool   unit_free = unit_taken[n] == 0;
        bool         sel       = unit_free;
        if (is_urllc) {
          delta[pair]           = static_cast<float>(remaining[u]);
          const bool power_ok   = pair_power[pair] <= remaining_power;
          const bool demand_ok  = remaining[u] > 0.0;
          sel                   = unit_free and power_ok and demand_ok;
          if (not sel) {
            if (not unit_free) {
              ++power_pairs_rejected_unit_taken;
            } else if (not power_ok) {
              ++power_pairs_rejected_infeasible;
            } else {
              ++power_pairs_rejected_no_demand;
            }
          }
        } else {
          delta[pair] = static_cast<float>(std::max(0.0, R_MIN_BPS - user_rate[u]) / R_MIN_BPS);
        }
        mask[pair] = sel ? 1 : 0;
        if (sel) {
          ever_selectable[u] = 1;
        }
        any = any or sel;
      }
    }
    if (not any) {
      break;
    }

    c_vec[0] = static_cast<float>(is_urllc ? remaining_power : n_free);
    if (is_urllc) {
      c_vec[1] = static_cast<float>(n_free);
    }

    const std::vector<float>& logp = mdl->decode_step(h_prev.data(), c_vec.data(), delta.data(), mask.data(), ws);
    const int                 best = attn_sched::model::argmax(logp);
    if (best < 0) {
      break;
    }

    const unsigned u = static_cast<unsigned>(best) / n_units;
    const unsigned n = static_cast<unsigned>(best) % n_units;
    unit_taken[n]    = 1;
    alloc_count[u] += 1.0;
    ue_units[u].set(n);
    n_free -= 1.0;
    ++rollout_decode_steps;
    if (is_urllc) {
      remaining_power -= pair_power[best];
      remaining[u] = std::max(0.0, remaining[u] - urllc_rate_bits(alloc_count[u], unit_blocklength));
    } else {
      user_rate[u] += pair_rate[best];
    }

    const float* sel_row = ws.h_enc.data() + static_cast<size_t>(best) * mdl->d_model;
    std::copy(sel_row, sel_row + mdl->d_model, h_prev.begin());
  }

  if (is_urllc) {
    ++power_nof_rollouts;
    power_committed_w_sum += (P_MAX_W - remaining_power);
    power_remaining_w_sum += remaining_power;
    power_decode_steps_taken += rollout_decode_steps;
    power_decode_steps_total += n_units;
  }

  const unsigned group = std::max(1u, cfg.prb_group);
  for (unsigned u = 0; u != n_ues; ++u) {
    const double tie = std::min(1.0, static_cast<double>(ue_candidates[u].pending_bytes.value()) / 1e9);
    if (alloc_count[u] > 0.0) {
      ue_candidates[u].priority = alloc_count[u] + tie;

      ue_candidates[u].preferred_vrbs = vrb_bitmap(dedicated_bwp_rbs);
      for (int n = ue_units[u].find_lowest(true); n >= 0;
           n     = ue_units[u].find_lowest(static_cast<size_t>(n) + 1, n_units, true)) {
        const int      start         = static_cast<int>(static_cast<unsigned>(n) * group) + vrb_origin_offset;
        const int      stop          = static_cast<int>((static_cast<unsigned>(n) + 1) * group) + vrb_origin_offset;
        const unsigned clipped_start = static_cast<unsigned>(std::clamp(start, 0, static_cast<int>(dedicated_bwp_rbs)));
        const unsigned clipped_stop  = static_cast<unsigned>(std::clamp(stop, 0, static_cast<int>(dedicated_bwp_rbs)));
        if (clipped_stop > clipped_start) {
          ue_candidates[u].preferred_vrbs.fill(clipped_start, clipped_stop);
        }
      }
    } else if (ever_selectable[u] != 0) {
      ue_candidates[u].priority = forbid_sched_priority;
    } else {
      ue_candidates[u].priority = tie;
    }
  }
  return true;
}

void scheduler_attention_ml::compute_ue_dl_priorities(slot_point               pdcch_slot,
                                                      slot_point               pdsch_slot,
                                                      span<ue_newtx_candidate> ue_candidates)
{
  if (not enabled) {
    fallback.compute_ue_dl_priorities(pdcch_slot, pdsch_slot, ue_candidates);
    return;
  }

  const auto start = std::chrono::steady_clock::now();
  const bool ran   = run_rollout(ue_candidates);
  const auto us =
      std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - start).count();

  if (not ran) {
    fallback.compute_ue_dl_priorities(pdcch_slot, pdsch_slot, ue_candidates);
    return;
  }

  rollout_us_sum += static_cast<std::uint64_t>(us);
  rollout_us_max = std::max<unsigned>(rollout_us_max, static_cast<unsigned>(us));
  if (++rollout_count == stats_period) {
    ocudulog::fetch_basic_logger("SCHED").info("attention_ml [{}]: {} rollouts, avg {} us, max {} us, budget {} us.",
                                               cfg.role == attention_ml_scheduler_config::role_type::urllc ? "urllc"
                                                                                                           : "embb",
                                               rollout_count,
                                               rollout_us_sum / rollout_count,
                                               rollout_us_max,
                                               deadline_us);
    rollout_count  = 0;
    rollout_us_sum = 0;
    rollout_us_max = 0;
  }

  if (us <= deadline_us) {
    consecutive_misses = 0;
    return;
  }

  ++consecutive_misses;
  fallback.compute_ue_dl_priorities(pdcch_slot, pdsch_slot, ue_candidates);
  if (consecutive_misses >= cfg.max_deadline_misses) {
    enabled = false;
    ocudulog::fetch_basic_logger("SCHED").warning(
        "attention_ml: rollout took {} us against a {} us budget on {} consecutive slots. Model disabled; "
        "the QoS policy takes over. Increase prb_group to shrink the rollout.",
        us,
        deadline_us,
        consecutive_misses);
  }
}

scheduler_policy_power_stats scheduler_attention_ml::consume_power_stats()
{
  scheduler_policy_power_stats out{};
  if (power_nof_rollouts > 0) {
    out.has_stats               = true;
    out.nof_rollouts             = power_nof_rollouts;
    out.mean_power_committed_w   = power_committed_w_sum / power_nof_rollouts;
    out.mean_power_remaining_w   = power_remaining_w_sum / power_nof_rollouts;
    out.pairs_rejected_unit_taken       = power_pairs_rejected_unit_taken;
    out.pairs_rejected_power_infeasible = power_pairs_rejected_infeasible;
    out.pairs_rejected_no_demand        = power_pairs_rejected_no_demand;
    out.decode_steps_taken       = power_decode_steps_taken;
    out.decode_steps_total       = power_decode_steps_total;
  }

  power_nof_rollouts             = 0;
  power_committed_w_sum          = 0.0;
  power_remaining_w_sum          = 0.0;
  power_pairs_rejected_unit_taken = 0;
  power_pairs_rejected_infeasible = 0;
  power_pairs_rejected_no_demand  = 0;
  power_decode_steps_taken        = 0;
  power_decode_steps_total        = 0;
  return out;
}
