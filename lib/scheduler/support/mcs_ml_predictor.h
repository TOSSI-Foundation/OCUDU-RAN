// SPDX-FileCopyrightText: Copyright (C) 2021-2026 Software Radio Systems Limited
// SPDX-License-Identifier: BSD-3-Clause-Open-MPI
// Portions of this file may implement 3GPP specifications, which may be subject to additional licensing requirements.

#pragma once

#include "mcs_ml_model.inc"
#include "ocudu/ocudulog/ocudulog.h"
#include "ocudu/ran/sch/sch_mcs.h"
#include "ocudu/scheduler/config/scheduler_expert_config.h"
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <string>
#include <sys/stat.h>
#include <vector>

namespace ocudu {
namespace mcs_ml {

struct model {
  int                 num_features  = NUM_FEATURES;
  int                 num_trees     = 0;
  double              learning_rate = 0.0;
  double              init_logodds  = 0.0;
  std::vector<int>    tree_offset;
  std::vector<int>    node_feature;
  std::vector<double> node_threshold;
  std::vector<int>    node_left;
  std::vector<int>    node_right;
  std::vector<double> node_value;

  double predict_proba(const double* feat) const
  {
    double score = init_logodds;
    for (int t = 0; t < num_trees; ++t) {
      int node = tree_offset[t];
      while (node_left[node] >= 0) {
        node = (feat[node_feature[node]] <= node_threshold[node]) ? node_left[node] : node_right[node];
      }
      score += learning_rate * node_value[node];
    }
    return 1.0 / (1.0 + std::exp(-score));
  }
};

inline std::shared_ptr<const model> make_seed_model()
{
  auto m            = std::make_shared<model>();
  m->num_features   = NUM_FEATURES;
  m->num_trees      = NUM_TREES;
  m->learning_rate  = LEARNING_RATE;
  m->init_logodds   = INIT_LOGODDS;
  const int n_nodes = TREE_OFFSET[NUM_TREES];
  m->tree_offset.assign(TREE_OFFSET, TREE_OFFSET + NUM_TREES + 1);
  m->node_feature.assign(NODE_FEATURE, NODE_FEATURE + n_nodes);
  m->node_threshold.assign(NODE_THRESHOLD, NODE_THRESHOLD + n_nodes);
  m->node_left.assign(NODE_LEFT, NODE_LEFT + n_nodes);
  m->node_right.assign(NODE_RIGHT, NODE_RIGHT + n_nodes);
  m->node_value.assign(NODE_VALUE, NODE_VALUE + n_nodes);
  return m;
}

class predictor
{
public:
  static predictor& instance()
  {
    static predictor inst;
    return inst;
  }

  void configure(const ml_mcs_expert_config& cfg)
  {
    enabled_      = cfg.inference_enabled;
    bler_target_  = cfg.inference_bler_target;
    model_path_   = cfg.inference_model_path;
    revert_flag_  = cfg.revert_flag_path;
    log_decisions_ = cfg.inference_enabled;

    std::shared_ptr<const model> init = make_seed_model();
    if (!model_path_.empty()) {
      if (auto fm = load_model_file(model_path_); fm && fm->num_features == NUM_FEATURES) {
        init              = fm;
        last_model_mtime_.store(file_mtime(model_path_), std::memory_order_relaxed);
      }
    }
    std::atomic_store(&active_, init);
    configured_ = true;
  }

  bool   enabled() const { return enabled_; }
  double bler_target() const { return bler_target_; }

  sch_mcs_index select_mcs(unsigned      mcs_table,
                           unsigned      wideband_cqi,
                           double        pusch_avg_sinr_db,
                           double        ul_snr_offset_db,
                           int           olla_mcs,
                           sch_mcs_index mcs_lo,
                           sch_mcs_index mcs_hi,
                           sch_mcs_index fallback) const
  {
    if (revert_active()) {
      return fallback;
    }
    maybe_reload();

    std::shared_ptr<const model> m = std::atomic_load(&active_);

    double feat[NUM_FEATURES];
    feat[FEAT_MCS_TABLE]         = static_cast<double>(mcs_table);
    feat[FEAT_WIDEBAND_CQI]      = static_cast<double>(wideband_cqi);
    feat[FEAT_PUSCH_AVG_SINR_DB] = pusch_avg_sinr_db;
    feat[FEAT_UL_SNR_OFFSET_DB]  = ul_snr_offset_db;

    const double thresh = 1.0 - bler_target_;

    for (int mm = static_cast<int>(mcs_hi.value()); mm >= static_cast<int>(mcs_lo.value()); --mm) {
      feat[FEAT_MCS] = static_cast<double>(mm);
      const double p = m->predict_proba(feat);
      if (p >= thresh) {
        log_decision(olla_mcs, mm, p, pusch_avg_sinr_db, wideband_cqi, true);
        return sch_mcs_index{static_cast<uint8_t>(mm)};
      }
    }
    feat[FEAT_MCS] = static_cast<double>(fallback.value());
    log_decision(olla_mcs, fallback.value(), m->predict_proba(feat), pusch_avg_sinr_db, wideband_cqi, false);
    return fallback;
  }

  double predict_for(unsigned mcs,
                     unsigned mcs_table,
                     unsigned wideband_cqi,
                     double   pusch_avg_sinr_db,
                     double   ul_snr_offset_db,
                     int      olla_mcs) const
  {
    (void)olla_mcs;
    std::shared_ptr<const model> m = std::atomic_load(&active_);
    double                       feat[NUM_FEATURES];
    feat[FEAT_MCS]               = static_cast<double>(mcs);
    feat[FEAT_MCS_TABLE]         = static_cast<double>(mcs_table);
    feat[FEAT_WIDEBAND_CQI]      = static_cast<double>(wideband_cqi);
    feat[FEAT_PUSCH_AVG_SINR_DB] = pusch_avg_sinr_db;
    feat[FEAT_UL_SNR_OFFSET_DB]  = ul_snr_offset_db;
    return m->predict_proba(feat);
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
    if (!m || m->num_features != NUM_FEATURES) {
      return false;
    }
    std::atomic_store(&active_, std::shared_ptr<const model>(m));
    last_model_mtime_.store(mt, std::memory_order_relaxed);
    if (log_decisions_) {
      ocudulog::fetch_basic_logger("SCHED").info("ML_LA: hot-swapped model from {} ({} trees)",
                                                 model_path_, m->num_trees);
    }
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

  bool revert_active() const
  {
    if (revert_flag_.empty()) {
      return false;
    }
    static constexpr unsigned REVERT_POLL = 2000;
    if (revert_check_ctr_.fetch_add(1, std::memory_order_relaxed) % REVERT_POLL == 0) {
      if (FILE* f = std::fopen(revert_flag_.c_str(), "rb")) {
        std::fclose(f);
        revert_cached_.store(true, std::memory_order_relaxed);
      } else {
        revert_cached_.store(false, std::memory_order_relaxed);
      }
    }
    return revert_cached_.load(std::memory_order_relaxed);
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
    auto m            = std::make_shared<model>();
    m->num_features   = static_cast<int>(scalar(s, "num_features", NUM_FEATURES));
    m->num_trees      = static_cast<int>(scalar(s, "num_trees", 0));
    m->learning_rate  = scalar(s, "learning_rate", 0.0);
    m->init_logodds   = scalar(s, "init_logodds", 0.0);
    m->tree_offset    = int_array(s, "tree_offset");
    m->node_feature   = int_array(s, "node_feature");
    m->node_threshold = dbl_array(s, "node_threshold");
    m->node_left      = int_array(s, "node_left");
    m->node_right     = int_array(s, "node_right");
    m->node_value     = dbl_array(s, "node_value");
    if (m->num_trees <= 0 || static_cast<int>(m->tree_offset.size()) != m->num_trees + 1 ||
        m->node_feature.empty() || m->node_feature.size() != m->node_threshold.size() ||
        m->node_feature.size() != m->node_left.size() || m->node_feature.size() != m->node_right.size() ||
        m->node_feature.size() != m->node_value.size()) {
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

  static std::vector<int> int_array(const std::string& s, const char* key)
  {
    std::vector<double> d = dbl_array(s, key);
    std::vector<int>    out;
    out.reserve(d.size());
    for (double v : d) {
      out.push_back(static_cast<int>(v));
    }
    return out;
  }

  void log_decision(int olla_mcs, int ml_mcs, double p, double sinr_db, unsigned cqi, bool hit) const
  {
    if (!log_decisions_) {
      return;
    }
    ocudulog::fetch_basic_logger("SCHED").info(
        "ML_LA: olla_mcs={} ml_mcs={} delta={} p_succ={:.4f} bler_target={:.2f} sinr_db={:.1f} cqi={} src={}",
        olla_mcs, ml_mcs, ml_mcs - olla_mcs, p, bler_target_, sinr_db, cqi, hit ? "ml" : "olla_fallback");
  }

  bool        configured_    = false;
  bool        enabled_       = false;
  bool        log_decisions_ = false;
  double      bler_target_   = 0.1;
  std::string model_path_;
  std::string revert_flag_;

  mutable std::atomic<long>     last_model_mtime_{0};
  mutable std::atomic<unsigned> reload_check_ctr_{0};
  mutable std::atomic<unsigned> revert_check_ctr_{0};
  mutable std::atomic<bool>     revert_cached_{false};

  mutable std::shared_ptr<const model> active_;
};

} // namespace mcs_ml
} // namespace ocudu
