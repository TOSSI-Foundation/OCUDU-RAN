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

#include "bsr_periodicity_model.inc"
#include "ocudu/mac/bsr_config.h"
#include "ocudu/ocudulog/ocudulog.h"
#include "ocudu/scheduler/config/scheduler_expert_config.h"
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <deque>
#include <memory>
#include <string>
#include <sys/stat.h>
#include <vector>

namespace ocudu {
namespace bsr_ml {

struct model {
  unsigned            window_size   = 30;
  unsigned            n_components  = 100;
  double              gamma         = 1.0;

  std::vector<double> scaler_mean;
  std::vector<double> scaler_scale;

  std::vector<double> random_weights;
  std::vector<double> random_offset;
  std::vector<double> coef;
  double              intercept = 0.0;

  bool valid() const
  {
    return window_size > 0 && n_components > 0 && random_weights.size() == window_size * n_components &&
           random_offset.size() == n_components && coef.size() == n_components &&
           scaler_mean.size() == window_size && scaler_scale.size() == window_size;
  }

  double predict(const std::vector<double>& window) const
  {
    double y = intercept;
    for (unsigned j = 0; j != n_components; ++j) {
      double proj = random_offset[j];
      for (unsigned i = 0; i != window_size; ++i) {
        const double xi = (window[i] - scaler_mean[i]) / scaler_scale[i];
        proj += xi * random_weights[i * n_components + j];
      }
      const double z = std::cos(proj) * std::sqrt(2.0 / static_cast<double>(n_components));
      y += coef[j] * z;
    }
    return y;
  }
};

inline std::shared_ptr<const model> make_seed_model()
{
  auto m          = std::make_shared<model>();
  m->window_size  = SEED_WINDOW_SIZE;
  m->n_components = SEED_N_COMPONENTS;
  m->gamma        = SEED_GAMMA;
  m->intercept    = SEED_INTERCEPT;
  m->scaler_mean.assign(SEED_SCALER_MEAN, SEED_SCALER_MEAN + SEED_WINDOW_SIZE);
  m->scaler_scale.assign(SEED_SCALER_SCALE, SEED_SCALER_SCALE + SEED_WINDOW_SIZE);
  m->random_weights.assign(SEED_RANDOM_WEIGHTS, SEED_RANDOM_WEIGHTS + SEED_WINDOW_SIZE * SEED_N_COMPONENTS);
  m->random_offset.assign(SEED_RANDOM_OFFSET, SEED_RANDOM_OFFSET + SEED_N_COMPONENTS);
  m->coef.assign(SEED_COEF, SEED_COEF + SEED_N_COMPONENTS);
  return m;
}

inline periodic_bsr_timer map_to_nearest_periodicity(double predicted_subframes)
{
  static constexpr periodic_bsr_timer candidates[] = {periodic_bsr_timer::sf1,
                                                      periodic_bsr_timer::sf5,
                                                      periodic_bsr_timer::sf10,
                                                      periodic_bsr_timer::sf16,
                                                      periodic_bsr_timer::sf20,
                                                      periodic_bsr_timer::sf32,
                                                      periodic_bsr_timer::sf40,
                                                      periodic_bsr_timer::sf64,
                                                      periodic_bsr_timer::sf80,
                                                      periodic_bsr_timer::sf128,
                                                      periodic_bsr_timer::sf160,
                                                      periodic_bsr_timer::sf320,
                                                      periodic_bsr_timer::sf640,
                                                      periodic_bsr_timer::sf1280,
                                                      periodic_bsr_timer::sf2560};
  periodic_bsr_timer best         = candidates[0];
  double             best_dist    = std::abs(static_cast<double>(periodic_bsr_timer_to_value(best)) - predicted_subframes);
  for (periodic_bsr_timer c : candidates) {
    const double dist = std::abs(static_cast<double>(periodic_bsr_timer_to_value(c)) - predicted_subframes);
    if (dist < best_dist) {
      best_dist = dist;
      best      = c;
    }
  }
  return best;
}

class interarrival_window
{
public:
  explicit interarrival_window(unsigned window_size) : window_size_(window_size) {}

  void push(double interarrival_slots)
  {
    values_.push_back(interarrival_slots);
    while (values_.size() > window_size_) {
      values_.pop_front();
    }
  }

  bool full() const { return values_.size() == window_size_; }

  std::vector<double> to_vector() const { return std::vector<double>(values_.begin(), values_.end()); }

private:
  unsigned           window_size_;
  std::deque<double> values_;
};

class predictor
{
public:
  static predictor& instance()
  {
    static predictor inst;
    return inst;
  }

  void configure(const bsr_ml_expert_config& cfg)
  {
    enabled_    = cfg.inference_enabled;
    model_path_ = cfg.inference_model_path;

    std::shared_ptr<const model> init = make_seed_model();
    if (!model_path_.empty()) {
      if (auto m = load_model_file(model_path_); m && m->valid()) {
        init = m;
        last_model_mtime_.store(file_mtime(model_path_), std::memory_order_relaxed);
      }
    }
    std::atomic_store(&active_, init);
    configured_ = true;
  }

  bool enabled() const { return enabled_ && std::atomic_load(&active_) != nullptr; }

  unsigned window_size() const
  {
    std::shared_ptr<const model> m = std::atomic_load(&active_);
    return m ? m->window_size : 0;
  }

  struct prediction_result {
    bool               has_prediction = false;
    double             predicted_interarrival_slots = 0.0;
    periodic_bsr_timer mapped_periodicity           = periodic_bsr_timer::infinity;
  };

  prediction_result predict(const std::vector<double>& window, unsigned numerology) const
  {
    prediction_result res;
    if (!enabled()) {
      return res;
    }
    maybe_reload();
    std::shared_ptr<const model> m = std::atomic_load(&active_);
    if (!m || window.size() != m->window_size) {
      return res;
    }
    const double predicted_slots     = m->predict(window);
    const double predicted_subframes = predicted_slots / static_cast<double>(1u << numerology);
    res.has_prediction               = true;
    res.predicted_interarrival_slots = predicted_slots;
    res.mapped_periodicity           = map_to_nearest_periodicity(predicted_subframes);
    return res;
  }

  bool reload() const
  {
    if (model_path_.empty()) {
      return false;
    }
    long mt = file_mtime(model_path_);
    if (mt == 0 || mt == last_model_mtime_.load(std::memory_order_relaxed)) {
      return false;
    }
    auto m = load_model_file(model_path_);
    if (!m || !m->valid()) {
      return false;
    }
    std::atomic_store(&active_, std::shared_ptr<const model>(m));
    last_model_mtime_.store(mt, std::memory_order_relaxed);
    ocudulog::fetch_basic_logger("SCHED").info(
        "BSR_ML: hot-swapped periodicity model from {} (window_size={}, n_components={})",
        model_path_,
        m->window_size,
        m->n_components);
    return true;
  }

private:
  predictor() { std::atomic_store(&active_, make_seed_model()); }

  void maybe_reload() const
  {
    if (model_path_.empty()) {
      return;
    }
    static constexpr unsigned RELOAD_POLL = 2000;
    if (reload_check_ctr_.fetch_add(1, std::memory_order_relaxed) % RELOAD_POLL == 0) {
      reload();
    }
  }

  static long file_mtime(const std::string& path)
  {
    struct ::stat st;
    return (::stat(path.c_str(), &st) == 0) ? static_cast<long>(st.st_mtime) : 0L;
  }

  static std::shared_ptr<model> load_model_file(const std::string& path)
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
    auto m           = std::make_shared<model>();
    m->window_size   = static_cast<unsigned>(scalar(s, "window_size", 30.0));
    m->n_components  = static_cast<unsigned>(scalar(s, "n_components", 100.0));
    m->gamma         = scalar(s, "gamma", 1.0);
    m->scaler_mean    = dbl_array(s, "scaler_mean");
    m->scaler_scale   = dbl_array(s, "scaler_scale");
    m->random_weights = dbl_array(s, "random_weights");
    m->random_offset   = dbl_array(s, "random_offset");
    m->coef             = dbl_array(s, "coef");
    m->intercept         = scalar(s, "intercept", 0.0);
    if (!m->valid()) {
      return nullptr;
    }
    return m;
  }

  static double scalar(const std::string& s, const char* key, double dflt)
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

  static std::vector<double> dbl_array(const std::string& s, const char* key)
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

  bool        configured_ = false;
  bool        enabled_    = false;
  std::string model_path_;

  mutable std::atomic<long>     last_model_mtime_{0};
  mutable std::atomic<unsigned> reload_check_ctr_{0};

  mutable std::shared_ptr<const model> active_;
};

}
}
