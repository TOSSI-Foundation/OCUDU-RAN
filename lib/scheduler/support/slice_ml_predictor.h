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

#include "slice_ml_actor_model.inc"
#include "ocudu/ocudulog/ocudulog.h"
#include "ocudu/scheduler/config/scheduler_expert_config.h"
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <string>
#include <sys/stat.h>
#include <vector>

namespace ocudu {
namespace slice_ml {

inline double sigmoid(double x)
{
  return 1.0 / (1.0 + std::exp(-x));
}

struct slice_ml_observation {
  double embb_throughput_mbps = 0.0;
  double urllc_demand_bytes = 0.0;
  double embb_ssr = 0.0;
  double urllc_ssr = 0.0;

  static constexpr unsigned NOF_FEATURES = 4;

  void to_features(std::vector<double>& out) const
  {
    out.push_back(embb_throughput_mbps);
    out.push_back(urllc_demand_bytes);
    out.push_back(embb_ssr);
    out.push_back(urllc_ssr);
  }
};

struct actor_model {
  unsigned n_in = ACTOR_SEED_N_IN;
  unsigned d_hidden = ACTOR_SEED_D_HIDDEN;
  unsigned d_fc = ACTOR_SEED_D_FC;
  unsigned n_actions = ACTOR_SEED_N_ACTIONS;

  std::vector<double> feat_mean;
  std::vector<double> feat_scale;

  std::vector<double> w_i, u_i, b_i;
  std::vector<double> w_f, u_f, b_f;
  std::vector<double> w_g, u_g, b_g;
  std::vector<double> w_o, u_o, b_o;

  std::vector<double> w_fc, b_fc;
  std::vector<double> w_out, b_out;

  bool valid() const
  {
    const size_t wsz = static_cast<size_t>(d_hidden) * n_in;
    const size_t usz = static_cast<size_t>(d_hidden) * d_hidden;
    return n_in > 0 && d_hidden > 0 && d_fc > 0 && n_actions > 0 && feat_mean.size() == n_in &&
           feat_scale.size() == n_in && w_i.size() == wsz && u_i.size() == usz && b_i.size() == d_hidden &&
           w_f.size() == wsz && u_f.size() == usz && b_f.size() == d_hidden && w_g.size() == wsz &&
           u_g.size() == usz && b_g.size() == d_hidden && w_o.size() == wsz && u_o.size() == usz &&
           b_o.size() == d_hidden && w_fc.size() == static_cast<size_t>(d_fc) * d_hidden && b_fc.size() == d_fc &&
           w_out.size() == static_cast<size_t>(n_actions) * d_fc && b_out.size() == n_actions;
  }

  void lstm_step(const std::vector<double>& x, std::vector<double>& h, std::vector<double>& c) const
  {
    std::vector<double> xs(n_in);
    for (unsigned k = 0; k != n_in; ++k) {
      const double sc = (feat_scale[k] != 0.0) ? feat_scale[k] : 1.0;
      xs[k]           = (x[k] - feat_mean[k]) / sc;
    }

    std::vector<double> h_new(d_hidden);
    for (unsigned j = 0; j != d_hidden; ++j) {
      double ia = b_i[j], fa = b_f[j], ga = b_g[j], oa = b_o[j];
      for (unsigned k = 0; k != n_in; ++k) {
        const size_t w = static_cast<size_t>(j) * n_in + k;
        ia += w_i[w] * xs[k];
        fa += w_f[w] * xs[k];
        ga += w_g[w] * xs[k];
        oa += w_o[w] * xs[k];
      }
      for (unsigned k = 0; k != d_hidden; ++k) {
        const size_t u = static_cast<size_t>(j) * d_hidden + k;
        ia += u_i[u] * h[k];
        fa += u_f[u] * h[k];
        ga += u_g[u] * h[k];
        oa += u_o[u] * h[k];
      }
      const double i_g = sigmoid(ia);
      const double f_g = sigmoid(fa);
      const double g_g = std::tanh(ga);
      const double o_g = sigmoid(oa);
      c[j]             = f_g * c[j] + i_g * g_g;
      h_new[j]         = o_g * std::tanh(c[j]);
    }
    h.swap(h_new);
  }

  std::vector<double> logits_from_hidden(const std::vector<double>& h) const
  {
    std::vector<double> fc(d_fc);
    for (unsigned j = 0; j != d_fc; ++j) {
      double acc = b_fc[j];
      for (unsigned k = 0; k != d_hidden; ++k) {
        acc += w_fc[static_cast<size_t>(j) * d_hidden + k] * h[k];
      }
      fc[j] = acc > 0.0 ? acc : 0.0;
    }
    std::vector<double> logits(n_actions);
    for (unsigned a = 0; a != n_actions; ++a) {
      double acc = b_out[a];
      for (unsigned j = 0; j != d_fc; ++j) {
        acc += w_out[static_cast<size_t>(a) * d_fc + j] * fc[j];
      }
      logits[a] = acc;
    }
    return logits;
  }

  static std::vector<double> softmax(const std::vector<double>& logits)
  {
    std::vector<double> p(logits.size());
    if (logits.empty()) {
      return p;
    }
    double mx = logits[0];
    for (double v : logits) {
      mx = v > mx ? v : mx;
    }
    double sum = 0.0;
    for (size_t i = 0; i != logits.size(); ++i) {
      p[i] = std::exp(logits[i] - mx);
      sum += p[i];
    }
    if (sum > 0.0) {
      for (double& v : p) {
        v /= sum;
      }
    }
    return p;
  }
};

inline std::shared_ptr<const actor_model> make_actor_seed()
{
  auto m       = std::make_shared<actor_model>();
  m->n_in      = ACTOR_SEED_N_IN;
  m->d_hidden  = ACTOR_SEED_D_HIDDEN;
  m->d_fc      = ACTOR_SEED_D_FC;
  m->n_actions = ACTOR_SEED_N_ACTIONS;

  const size_t wsz = static_cast<size_t>(ACTOR_SEED_D_HIDDEN) * ACTOR_SEED_N_IN;
  const size_t usz = static_cast<size_t>(ACTOR_SEED_D_HIDDEN) * ACTOR_SEED_D_HIDDEN;

  m->feat_mean.assign(ACTOR_SEED_FEAT_MEAN, ACTOR_SEED_FEAT_MEAN + ACTOR_SEED_N_IN);
  m->feat_scale.assign(ACTOR_SEED_FEAT_SCALE, ACTOR_SEED_FEAT_SCALE + ACTOR_SEED_N_IN);

  m->w_i.assign(wsz, 0.0);
  m->u_i.assign(usz, 0.0);
  m->b_i.assign(ACTOR_SEED_D_HIDDEN, 0.0);
  m->w_f.assign(wsz, 0.0);
  m->u_f.assign(usz, 0.0);
  m->b_f.assign(ACTOR_SEED_D_HIDDEN, 0.0);
  m->w_g.assign(wsz, 0.0);
  m->u_g.assign(usz, 0.0);
  m->b_g.assign(ACTOR_SEED_D_HIDDEN, 0.0);
  m->w_o.assign(wsz, 0.0);
  m->u_o.assign(usz, 0.0);
  m->b_o.assign(ACTOR_SEED_D_HIDDEN, 0.0);
  m->w_fc.assign(static_cast<size_t>(ACTOR_SEED_D_FC) * ACTOR_SEED_D_HIDDEN, 0.0);
  m->b_fc.assign(ACTOR_SEED_D_FC, 0.0);
  m->w_out.assign(static_cast<size_t>(ACTOR_SEED_N_ACTIONS) * ACTOR_SEED_D_FC, 0.0);
  m->b_out.assign(ACTOR_SEED_B_OUT, ACTOR_SEED_B_OUT + ACTOR_SEED_N_ACTIONS);
  return m;
}

inline double parse_scalar_field(const std::string& s, const char* key, double dflt)
{
  const std::string k   = std::string("\"") + key + "\"";
  auto              pos = s.find(k);
  if (pos == std::string::npos) {
    return dflt;
  }
  pos = s.find(':', pos);
  if (pos == std::string::npos) {
    return dflt;
  }
  return std::atof(s.c_str() + pos + 1);
}

inline std::vector<double> parse_array_field(const std::string& s, const char* key)
{
  std::vector<double> out;
  const std::string   k   = std::string("\"") + key + "\"";
  auto                pos = s.find(k);
  if (pos == std::string::npos) {
    return out;
  }
  auto lb = s.find('[', pos);
  auto rb = s.find(']', lb);
  if (lb == std::string::npos || rb == std::string::npos) {
    return out;
  }
  const char* p   = s.c_str() + lb + 1;
  const char* end = s.c_str() + rb;
  while (p < end) {
    char*  np = nullptr;
    double v  = std::strtod(p, &np);
    if (np == p) {
      ++p;
      continue;
    }
    out.push_back(v);
    p = np;
    while (p < end && (*p == ',' || *p == ' ' || *p == '\n' || *p == '\t')) {
      ++p;
    }
  }
  return out;
}

inline std::shared_ptr<actor_model> load_actor_model_file(const std::string& path)
{
  FILE* fh = std::fopen(path.c_str(), "rb");
  if (!fh) {
    return nullptr;
  }
  std::string s;
  {
    char   buf[1 << 16];
    size_t n = 0;
    while ((n = std::fread(buf, 1, sizeof(buf), fh)) > 0) {
      s.append(buf, n);
    }
    std::fclose(fh);
  }
  auto m        = std::make_shared<actor_model>();
  m->n_in       = static_cast<unsigned>(parse_scalar_field(s, "n_in", 0.0));
  m->d_hidden   = static_cast<unsigned>(parse_scalar_field(s, "d_hidden", 0.0));
  m->d_fc       = static_cast<unsigned>(parse_scalar_field(s, "d_fc", 0.0));
  m->n_actions  = static_cast<unsigned>(parse_scalar_field(s, "n_actions", 0.0));
  m->feat_mean  = parse_array_field(s, "feat_mean");
  m->feat_scale = parse_array_field(s, "feat_scale");
  m->w_i        = parse_array_field(s, "w_i");
  m->u_i        = parse_array_field(s, "u_i");
  m->b_i        = parse_array_field(s, "b_i");
  m->w_f        = parse_array_field(s, "w_f");
  m->u_f        = parse_array_field(s, "u_f");
  m->b_f        = parse_array_field(s, "b_f");
  m->w_g        = parse_array_field(s, "w_g");
  m->u_g        = parse_array_field(s, "u_g");
  m->b_g        = parse_array_field(s, "b_g");
  m->w_o        = parse_array_field(s, "w_o");
  m->u_o        = parse_array_field(s, "u_o");
  m->b_o        = parse_array_field(s, "b_o");
  m->w_fc       = parse_array_field(s, "w_fc");
  m->b_fc       = parse_array_field(s, "b_fc");
  m->w_out      = parse_array_field(s, "w_out");
  m->b_out      = parse_array_field(s, "b_out");
  return m->valid() ? m : nullptr;
}

class predictor
{
public:
  static predictor& instance()
  {
    static predictor inst;
    return inst;
  }

  void configure(const slice_ml_expert_config& cfg)
  {
    enabled_    = cfg.inference_enabled;
    model_path_ = cfg.inference_model_path;

    std::shared_ptr<const actor_model> init = make_actor_seed();
    if (!model_path_.empty()) {
      if (auto m = load_model_file(model_path_); m && m->valid()) {
        init = m;
        last_model_mtime_.store(file_mtime(model_path_), std::memory_order_relaxed);
        ocudulog::fetch_basic_logger("SCHED").info(
            "SLICE_ML: loaded actor model from {} (n_in={}, d_hidden={}, d_fc={}, n_actions={})",
            model_path_,
            m->n_in,
            m->d_hidden,
            m->d_fc,
            m->n_actions);
      } else {
        ocudulog::fetch_basic_logger("SCHED").warning(
            "SLICE_ML: could not load actor model from {}; falling back to the compiled seed, which "
            "always selects the default action (controller will not change any ratio)",
            model_path_);
      }
    }
    std::atomic_store(&active_, init);
    generation_.fetch_add(1, std::memory_order_relaxed);
    configured_ = true;
  }

  bool enabled() const { return enabled_ && std::atomic_load(&active_) != nullptr; }

  std::shared_ptr<const actor_model> model() const { return std::atomic_load(&active_); }

  unsigned generation() const { return generation_.load(std::memory_order_relaxed); }

  bool maybe_reload() const
  {
    if (model_path_.empty()) {
      return false;
    }
    const long mt = file_mtime(model_path_);
    if (mt == 0 || mt == last_model_mtime_.load(std::memory_order_relaxed)) {
      return false;
    }
    auto m = load_model_file(model_path_);
    if (!m || !m->valid()) {
      last_model_mtime_.store(mt, std::memory_order_relaxed);
      ocudulog::fetch_basic_logger("SCHED").warning("SLICE_ML: rejected invalid actor model at {}; keeping current",
                                                    model_path_);
      return false;
    }
    std::atomic_store(&active_, std::shared_ptr<const actor_model>(m));
    last_model_mtime_.store(mt, std::memory_order_relaxed);
    generation_.fetch_add(1, std::memory_order_relaxed);
    ocudulog::fetch_basic_logger("SCHED").info("SLICE_ML: hot-swapped actor model from {} (d_hidden={}, n_actions={})",
                                               model_path_,
                                               m->d_hidden,
                                               m->n_actions);
    return true;
  }

private:
  predictor() { std::atomic_store(&active_, make_actor_seed()); }

  static long file_mtime(const std::string& path)
  {
    struct ::stat st;
    return (::stat(path.c_str(), &st) == 0) ? static_cast<long>(st.st_mtime) : 0L;
  }

  static std::shared_ptr<actor_model> load_model_file(const std::string& path) { return load_actor_model_file(path); }

  bool        configured_ = false;
  bool        enabled_    = false;
  std::string model_path_;

  mutable std::atomic<long>     last_model_mtime_{0};
  mutable std::atomic<unsigned> generation_{0};

  mutable std::shared_ptr<const actor_model> active_;
};

}
}
