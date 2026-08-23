#pragma once

#include "csi_ml_gru_model.inc"
#include "csi_ml_wiener_model.inc"
#include "ocudu/ocudulog/ocudulog.h"
#include "ocudu/scheduler/config/scheduler_expert_config.h"
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <optional>
#include <string>
#include <sys/stat.h>
#include <vector>

namespace ocudu {
namespace csi_ml {

enum class model_kind { wiener, gru };

inline model_kind model_kind_from_string(const std::string& s)
{
  return (s == "gru") ? model_kind::gru : model_kind::wiener;
}

inline double sigmoid(double x)
{
  return 1.0 / (1.0 + std::exp(-x));
}

struct wiener_model {
  unsigned p     = WIENER_SEED_P;
  unsigned t_out = WIENER_SEED_T_OUT;
  double   mu    = WIENER_SEED_MU;
  double   sigma = WIENER_SEED_SIGMA;

  std::vector<double> coef;
  std::vector<double> intercept;

  bool valid() const
  {
    return p > 0 && t_out > 0 && sigma != 0.0 && coef.size() == static_cast<size_t>(t_out) * p &&
           intercept.size() == t_out;
  }

  std::vector<float> predict_vector(const std::vector<float>& window) const
  {
    std::vector<float> out;
    if (window.size() != p || !valid()) {
      return out;
    }

    std::vector<double> xs(p);
    for (unsigned i = 0; i != p; ++i) {
      xs[i] = (static_cast<double>(window[i]) - mu) / sigma;
    }
    out.resize(t_out);
    for (unsigned h = 0; h != t_out; ++h) {
      double acc = intercept[h];
      for (unsigned i = 0; i != p; ++i) {
        acc += coef[static_cast<size_t>(h) * p + i] * xs[i];
      }
      out[h] = static_cast<float>(acc * sigma + mu);
    }
    return out;
  }
};

struct gru_model {
  unsigned p     = GRU_SEED_P;
  unsigned d     = GRU_SEED_D;
  unsigned t_out = GRU_SEED_T_OUT;
  double   mu    = GRU_SEED_MU;
  double   sigma = GRU_SEED_SIGMA;

  std::vector<double> w_z, u_z, b_z;
  std::vector<double> w_r, u_r, b_r;
  std::vector<double> w_h, u_h, b_h;
  std::vector<double> w_out, b_out;

  bool valid() const
  {
    return p > 0 && d > 0 && t_out > 0 && sigma != 0.0 && w_z.size() == d && u_z.size() == size_t(d) * d &&
           b_z.size() == d && w_r.size() == d && u_r.size() == size_t(d) * d && b_r.size() == d &&
           w_h.size() == d && u_h.size() == size_t(d) * d && b_h.size() == d &&
           w_out.size() == size_t(t_out) * d && b_out.size() == t_out;
  }

  std::vector<float> predict_vector(const std::vector<float>& window) const
  {
    std::vector<float> out;
    if (window.size() != p || !valid()) {
      return out;
    }
    std::vector<double> h(d, 0.0), z(d), r(d), rh(d), hh(d), hnew(d);
    for (unsigned t = 0; t != p; ++t) {
      const double x = (static_cast<double>(window[t]) - mu) / sigma;

      for (unsigned j = 0; j != d; ++j) {
        double zacc = w_z[j] * x + b_z[j];
        double racc = w_r[j] * x + b_r[j];
        for (unsigned k = 0; k != d; ++k) {
          zacc += u_z[size_t(j) * d + k] * h[k];
          racc += u_r[size_t(j) * d + k] * h[k];
        }
        z[j] = sigmoid(zacc);
        r[j] = sigmoid(racc);
      }
      for (unsigned j = 0; j != d; ++j) {
        rh[j] = r[j] * h[j];
      }

      for (unsigned j = 0; j != d; ++j) {
        double hacc = w_h[j] * x + b_h[j];
        for (unsigned k = 0; k != d; ++k) {
          hacc += u_h[size_t(j) * d + k] * rh[k];
        }
        hh[j] = std::tanh(hacc);
      }
      for (unsigned j = 0; j != d; ++j) {
        hnew[j] = (1.0 - z[j]) * h[j] + z[j] * hh[j];
      }
      h.swap(hnew);
    }
    out.resize(t_out);
    for (unsigned o = 0; o != t_out; ++o) {
      double y = b_out[o];
      for (unsigned j = 0; j != d; ++j) {
        y += w_out[size_t(o) * d + j] * h[j];
      }
      out[o] = static_cast<float>(y * sigma + mu);
    }
    return out;
  }
};

inline std::shared_ptr<const wiener_model> make_wiener_seed()
{
  auto m       = std::make_shared<wiener_model>();
  m->p         = WIENER_SEED_P;
  m->t_out     = WIENER_SEED_T_OUT;
  m->mu        = WIENER_SEED_MU;
  m->sigma     = WIENER_SEED_SIGMA;
  m->coef.assign(WIENER_SEED_COEF, WIENER_SEED_COEF + size_t(WIENER_SEED_T_OUT) * WIENER_SEED_P);
  m->intercept.assign(WIENER_SEED_INTERCEPT, WIENER_SEED_INTERCEPT + WIENER_SEED_T_OUT);
  return m;
}

inline std::shared_ptr<const gru_model> make_gru_seed()
{
  auto m   = std::make_shared<gru_model>();
  m->p     = GRU_SEED_P;
  m->d     = GRU_SEED_D;
  m->t_out = GRU_SEED_T_OUT;
  m->mu    = GRU_SEED_MU;
  m->sigma = GRU_SEED_SIGMA;
  m->w_z.assign(GRU_SEED_W_Z, GRU_SEED_W_Z + GRU_SEED_D);
  m->u_z.assign(GRU_SEED_U_Z, GRU_SEED_U_Z + size_t(GRU_SEED_D) * GRU_SEED_D);
  m->b_z.assign(GRU_SEED_B_Z, GRU_SEED_B_Z + GRU_SEED_D);
  m->w_r.assign(GRU_SEED_W_R, GRU_SEED_W_R + GRU_SEED_D);
  m->u_r.assign(GRU_SEED_U_R, GRU_SEED_U_R + size_t(GRU_SEED_D) * GRU_SEED_D);
  m->b_r.assign(GRU_SEED_B_R, GRU_SEED_B_R + GRU_SEED_D);
  m->w_h.assign(GRU_SEED_W_H, GRU_SEED_W_H + GRU_SEED_D);
  m->u_h.assign(GRU_SEED_U_H, GRU_SEED_U_H + size_t(GRU_SEED_D) * GRU_SEED_D);
  m->b_h.assign(GRU_SEED_B_H, GRU_SEED_B_H + GRU_SEED_D);
  m->w_out.assign(GRU_SEED_W_OUT, GRU_SEED_W_OUT + size_t(GRU_SEED_T_OUT) * GRU_SEED_D);
  m->b_out.assign(GRU_SEED_B_OUT, GRU_SEED_B_OUT + GRU_SEED_T_OUT);
  return m;
}

namespace detail {

inline double json_scalar(const std::string& s, const char* key, double dflt)
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

inline std::vector<double> json_dbl_array(const std::string& s, const char* key)
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

inline bool read_file(const std::string& path, std::string& s)
{
  FILE* fh = std::fopen(path.c_str(), "rb");
  if (!fh) {
    return false;
  }
  char   buf[1 << 16];
  size_t n = 0;
  while ((n = std::fread(buf, 1, sizeof(buf), fh)) > 0) {
    s.append(buf, n);
  }
  std::fclose(fh);
  return true;
}

}

inline std::shared_ptr<wiener_model> load_wiener_file(const std::string& path)
{
  std::string s;
  if (!detail::read_file(path, s)) {
    return nullptr;
  }
  auto m       = std::make_shared<wiener_model>();
  m->p         = static_cast<unsigned>(detail::json_scalar(s, "p", WIENER_SEED_P));
  m->t_out     = static_cast<unsigned>(detail::json_scalar(s, "t_out", WIENER_SEED_T_OUT));
  m->mu        = detail::json_scalar(s, "mu", 0.0);
  m->sigma     = detail::json_scalar(s, "sigma", 1.0);
  m->coef      = detail::json_dbl_array(s, "coef");
  m->intercept = detail::json_dbl_array(s, "intercept");
  return m->valid() ? m : nullptr;
}

inline std::shared_ptr<gru_model> load_gru_file(const std::string& path)
{
  std::string s;
  if (!detail::read_file(path, s)) {
    return nullptr;
  }
  auto m   = std::make_shared<gru_model>();
  m->p     = static_cast<unsigned>(detail::json_scalar(s, "p", GRU_SEED_P));
  m->d     = static_cast<unsigned>(detail::json_scalar(s, "d", GRU_SEED_D));
  m->t_out = static_cast<unsigned>(detail::json_scalar(s, "t_out", GRU_SEED_T_OUT));
  m->mu    = detail::json_scalar(s, "mu", 0.0);
  m->sigma = detail::json_scalar(s, "sigma", 1.0);
  m->w_z   = detail::json_dbl_array(s, "w_z");
  m->u_z   = detail::json_dbl_array(s, "u_z");
  m->b_z   = detail::json_dbl_array(s, "b_z");
  m->w_r   = detail::json_dbl_array(s, "w_r");
  m->u_r   = detail::json_dbl_array(s, "u_r");
  m->b_r   = detail::json_dbl_array(s, "b_r");
  m->w_h   = detail::json_dbl_array(s, "w_h");
  m->u_h   = detail::json_dbl_array(s, "u_h");
  m->b_h   = detail::json_dbl_array(s, "b_h");
  m->w_out = detail::json_dbl_array(s, "w_out");
  m->b_out = detail::json_dbl_array(s, "b_out");
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

  void configure(const csi_ml_expert_config& cfg)
  {
    enabled_       = cfg.inference_enabled;
    apply_to_mcs_  = cfg.inference_apply_to_mcs;
    kind_          = model_kind_from_string(cfg.inference_model_type);
    wiener_path_   = cfg.inference_wiener_model_path;
    gru_path_      = cfg.inference_gru_model_path;

    if (kind_ == model_kind::wiener) {
      std::shared_ptr<const wiener_model> init = make_wiener_seed();
      if (!wiener_path_.empty()) {
        if (auto m = load_wiener_file(wiener_path_); m && m->valid()) {
          init = m;
          last_mtime_.store(file_mtime(wiener_path_), std::memory_order_relaxed);
        }
      }
      std::atomic_store(&wiener_active_, init);
    } else {
      std::shared_ptr<const gru_model> init = make_gru_seed();
      if (!gru_path_.empty()) {
        if (auto m = load_gru_file(gru_path_); m && m->valid()) {
          init = m;
          last_mtime_.store(file_mtime(gru_path_), std::memory_order_relaxed);
        }
      }
      std::atomic_store(&gru_active_, init);
    }
    configured_ = true;
  }

  bool enabled() const
  {
    if (!enabled_) {
      return false;
    }
    return (kind_ == model_kind::wiener) ? (std::atomic_load(&wiener_active_) != nullptr)
                                         : (std::atomic_load(&gru_active_) != nullptr);
  }

  bool apply_to_mcs() const { return enabled_ && apply_to_mcs_; }

  model_kind kind() const { return kind_; }

  unsigned window_size() const
  {
    if (kind_ == model_kind::wiener) {
      auto m = std::atomic_load(&wiener_active_);
      return m ? m->p : 0;
    }
    auto m = std::atomic_load(&gru_active_);
    return m ? m->p : 0;
  }

  std::vector<float> predict_vector(const std::vector<float>& window) const
  {
    if (!enabled()) {
      return {};
    }
    maybe_reload();
    if (kind_ == model_kind::wiener) {
      auto m = std::atomic_load(&wiener_active_);
      return m ? m->predict_vector(window) : std::vector<float>{};
    }
    auto m = std::atomic_load(&gru_active_);
    return m ? m->predict_vector(window) : std::vector<float>{};
  }

  std::optional<float> predict_next(const std::vector<float>& window) const
  {
    std::vector<float> v = predict_vector(window);
    if (v.empty()) {
      return std::nullopt;
    }
    return v.front();
  }

private:
  predictor()
  {
    std::atomic_store(&wiener_active_, std::shared_ptr<const wiener_model>(make_wiener_seed()));
    std::atomic_store(&gru_active_, std::shared_ptr<const gru_model>(make_gru_seed()));
  }

  void maybe_reload() const
  {
    const std::string& path = (kind_ == model_kind::wiener) ? wiener_path_ : gru_path_;
    if (path.empty()) {
      return;
    }
    static constexpr unsigned RELOAD_POLL = 2000;
    if (reload_ctr_.fetch_add(1, std::memory_order_relaxed) % RELOAD_POLL == 0) {
      reload();
    }
  }

  bool reload() const
  {
    const std::string& path = (kind_ == model_kind::wiener) ? wiener_path_ : gru_path_;
    if (path.empty()) {
      return false;
    }
    long mt = file_mtime(path);
    if (mt == 0 || mt == last_mtime_.load(std::memory_order_relaxed)) {
      return false;
    }
    if (kind_ == model_kind::wiener) {
      auto m = load_wiener_file(path);
      if (!m || !m->valid()) {
        return false;
      }
      std::atomic_store(&wiener_active_, std::shared_ptr<const wiener_model>(m));
    } else {
      auto m = load_gru_file(path);
      if (!m || !m->valid()) {
        return false;
      }
      std::atomic_store(&gru_active_, std::shared_ptr<const gru_model>(m));
    }
    last_mtime_.store(mt, std::memory_order_relaxed);
    ocudulog::fetch_basic_logger("SCHED").info("CSI_ML: hot-swapped {} model from {}",
                                                (kind_ == model_kind::wiener) ? "wiener" : "gru",
                                                path);
    return true;
  }

  static long file_mtime(const std::string& path)
  {
    struct ::stat st;
    return (::stat(path.c_str(), &st) == 0) ? static_cast<long>(st.st_mtime) : 0L;
  }

  bool        configured_   = false;
  bool        enabled_      = false;
  bool        apply_to_mcs_ = false;
  model_kind  kind_         = model_kind::wiener;
  std::string wiener_path_;
  std::string gru_path_;

  mutable std::atomic<long>     last_mtime_{0};
  mutable std::atomic<unsigned> reload_ctr_{0};

  mutable std::shared_ptr<const wiener_model> wiener_active_;
  mutable std::shared_ptr<const gru_model>    gru_active_;
};

}
}
