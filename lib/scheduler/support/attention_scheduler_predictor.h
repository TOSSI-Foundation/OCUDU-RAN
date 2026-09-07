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

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <limits>
#include <memory>
#include <string>
#include <vector>

namespace ocudu {
namespace attn_sched {

inline float dot(const float* a, const float* b, unsigned n)
{
  float    acc[8] = {0.f, 0.f, 0.f, 0.f, 0.f, 0.f, 0.f, 0.f};
  unsigned i      = 0;
  for (; i + 8 <= n; i += 8) {
    for (unsigned l = 0; l != 8; ++l) {
      acc[l] += a[i + l] * b[i + l];
    }
  }
  float s = ((acc[0] + acc[1]) + (acc[2] + acc[3])) + ((acc[4] + acc[5]) + (acc[6] + acc[7]));
  for (; i != n; ++i) {
    s += a[i] * b[i];
  }
  return s;
}

inline void matvec(const float* W, const float* b, const float* x, float* y, unsigned rows, unsigned cols)
{
  for (unsigned r = 0; r != rows; ++r) {
    y[r] = b[r] + dot(W + static_cast<size_t>(r) * cols, x, cols);
  }
}

struct model;

struct workspace {
  std::vector<float> h, att, ctx, pq, pk, pv;
  std::vector<float> ff_tmp;
  std::vector<float> h_enc;
  std::vector<float> kv_keys, kv_values;
  std::vector<float> mean, q, qd, g;
  std::vector<float> w, logits, logp;
  unsigned           k = 0;

  void reserve(unsigned d_model, unsigned max_k)
  {
    const size_t kd = static_cast<size_t>(max_k) * d_model;
    for (auto* v : {&h, &att, &ctx, &pq, &pk, &pv, &h_enc, &kv_keys, &kv_values}) {
      v->reserve(kd);
    }
    ff_tmp.reserve(4u * d_model);
    for (auto* v : {&mean, &q, &qd, &g}) {
      v->reserve(d_model);
    }
    for (auto* v : {&w, &logits, &logp}) {
      v->reserve(max_k);
    }
  }
};

struct model {
  unsigned n_feat  = 0;
  unsigned n_ctx   = 0;
  unsigned d_model = 0;
  unsigned n_heads = 0;

  std::string role;
  unsigned prb_group   = 0;
  double   cell_bw_mhz = 0.0;

  std::vector<float> enc_proj_w, enc_proj_b;
  std::vector<float> enc_attn_in_w, enc_attn_in_b, enc_attn_out_w, enc_attn_out_b;
  std::vector<float> enc_norm1_w, enc_norm1_b, enc_norm2_w, enc_norm2_b;
  std::vector<float> enc_ff1_w, enc_ff1_b, enc_ff2_w, enc_ff2_b;

  std::vector<float> dec_ctx_w, dec_ctx_b;
  std::vector<float> dec_attn_in_w, dec_attn_in_b, dec_attn_out_w, dec_attn_out_b;
  std::vector<float> dec_delta_w, dec_delta_b;

  bool valid() const
  {
    const size_t d = d_model, ff = 4u * d_model;
    return n_feat > 0 && d_model > 0 && n_heads > 0 && d_model % n_heads == 0 &&
           enc_proj_w.size() == d * n_feat && enc_proj_b.size() == d &&
           enc_attn_in_w.size() == 3 * d * d && enc_attn_in_b.size() == 3 * d &&
           enc_attn_out_w.size() == d * d && enc_attn_out_b.size() == d && enc_norm1_w.size() == d &&
           enc_norm1_b.size() == d && enc_norm2_w.size() == d && enc_norm2_b.size() == d &&
           enc_ff1_w.size() == ff * d && enc_ff1_b.size() == ff && enc_ff2_w.size() == d * ff &&
           enc_ff2_b.size() == d && dec_ctx_w.size() == d * (2 * d + n_ctx) && dec_ctx_b.size() == d &&
           dec_attn_in_w.size() == 3 * d * d && dec_attn_in_b.size() == 3 * d &&
           dec_attn_out_w.size() == d * d && dec_attn_out_b.size() == d && dec_delta_w.size() == d &&
           dec_delta_b.size() == d;
  }

  void encode(const float* phi, unsigned k, workspace& ws) const
  {
    const unsigned d = d_model;
    const size_t   kd = static_cast<size_t>(k) * d;
    ws.k = k;
    ws.h.resize(kd);
    for (unsigned i = 0; i != k; ++i) {
      matvec(enc_proj_w.data(), enc_proj_b.data(), phi + static_cast<size_t>(i) * n_feat,
             ws.h.data() + static_cast<size_t>(i) * d, d, n_feat);
    }

    self_attention(ws);
    for (size_t i = 0; i != kd; ++i) {
      ws.att[i] += ws.h[i];
    }
    layer_norm(ws.att.data(), k, enc_norm1_w, enc_norm1_b);

    const unsigned ff = 4u * d;
    ws.h_enc.resize(kd);
    ws.ff_tmp.resize(ff);
    for (unsigned i = 0; i != k; ++i) {
      const float* row = ws.att.data() + static_cast<size_t>(i) * d;
      matvec(enc_ff1_w.data(), enc_ff1_b.data(), row, ws.ff_tmp.data(), ff, d);
      for (unsigned j = 0; j != ff; ++j) {
        ws.ff_tmp[j] = ws.ff_tmp[j] > 0.f ? ws.ff_tmp[j] : 0.f;
      }
      float* out = ws.h_enc.data() + static_cast<size_t>(i) * d;
      matvec(enc_ff2_w.data(), enc_ff2_b.data(), ws.ff_tmp.data(), out, d, ff);
      for (unsigned j = 0; j != d; ++j) {
        out[j] += row[j];
      }
    }
    layer_norm(ws.h_enc.data(), k, enc_norm2_w, enc_norm2_b);
  }

  void prepare_decode(workspace& ws) const
  {
    project_block(ws.h_enc.data(), ws.k, 1, dec_attn_in_w, dec_attn_in_b, ws.kv_keys);
    project_block(ws.h_enc.data(), ws.k, 2, dec_attn_in_w, dec_attn_in_b, ws.kv_values);
  }

  const std::vector<float>& decode_step(const float*   h_prev,
                                        const float*   c_vec,
                                        const float*   delta,
                                        const uint8_t* mask,
                                        workspace&     ws) const
  {
    const unsigned d = d_model;
    const unsigned k = ws.k;

    ws.mean.assign(d, 0.f);
    for (unsigned i = 0; i != k; ++i) {
      const float* row = ws.h_enc.data() + static_cast<size_t>(i) * d;
      for (unsigned j = 0; j != d; ++j) {
        ws.mean[j] += row[j];
      }
    }
    if (k > 0) {
      const float inv_k = 1.f / static_cast<float>(k);
      for (float& v : ws.mean) {
        v *= inv_k;
      }
    }
    const unsigned ctx_in = 2u * d + n_ctx;
    ws.q.resize(d);
    for (unsigned j = 0; j != d; ++j) {
      const float* row = dec_ctx_w.data() + static_cast<size_t>(j) * ctx_in;
      float        acc = dec_ctx_b[j] + dot(row, ws.mean.data(), d) + dot(row + d, h_prev, d);
      for (unsigned c = 0; c != n_ctx; ++c) {
        acc += row[2 * d + c] * c_vec[c];
      }
      ws.q[j] = acc;
    }

    cross_attention(mask, ws);

    ws.logits.resize(k);
    const float g_dw    = dot(dec_delta_w.data(), ws.g.data(), d);
    const float g_db    = dot(dec_delta_b.data(), ws.g.data(), d);
    const float inv_scl = 1.f / std::sqrt(static_cast<float>(d));
    for (unsigned i = 0; i != k; ++i) {
      const float he_g = dot(ws.h_enc.data() + static_cast<size_t>(i) * d, ws.g.data(), d);
      ws.logits[i]     = (he_g + delta[i] * g_dw + g_db) * inv_scl;
    }
    masked_log_softmax(mask, ws);
    return ws.logp;
  }

  static int argmax(const std::vector<float>& logp)
  {
    int   best = -1;
    float bv   = -std::numeric_limits<float>::infinity();
    for (size_t i = 0; i != logp.size(); ++i) {
      if (std::isfinite(logp[i]) && logp[i] > bv) {
        bv   = logp[i];
        best = static_cast<int>(i);
      }
    }
    return best;
  }

private:
  void project_block(const float*              src,
                     unsigned                  n,
                     unsigned                  block,
                     const std::vector<float>& in_w,
                     const std::vector<float>& in_b,
                     std::vector<float>&       dst) const
  {
    const unsigned d = d_model;
    dst.resize(static_cast<size_t>(n) * d);
    for (unsigned i = 0; i != n; ++i) {
      matvec(in_w.data() + static_cast<size_t>(block) * d * d,
             in_b.data() + static_cast<size_t>(block) * d,
             src + static_cast<size_t>(i) * d,
             dst.data() + static_cast<size_t>(i) * d,
             d,
             d);
    }
  }

  void self_attention(workspace& ws) const
  {
    const unsigned k = ws.k;
    project_block(ws.h.data(), k, 0, enc_attn_in_w, enc_attn_in_b, ws.pq);
    project_block(ws.h.data(), k, 1, enc_attn_in_w, enc_attn_in_b, ws.pk);
    project_block(ws.h.data(), k, 2, enc_attn_in_w, enc_attn_in_b, ws.pv);
    attention_core(ws.pq.data(), k, nullptr, ws);
    ws.att.resize(static_cast<size_t>(k) * d_model);
    for (unsigned i = 0; i != k; ++i) {
      matvec(enc_attn_out_w.data(),
             enc_attn_out_b.data(),
             ws.ctx.data() + static_cast<size_t>(i) * d_model,
             ws.att.data() + static_cast<size_t>(i) * d_model,
             d_model,
             d_model);
    }
  }

  void cross_attention(const uint8_t* mask, workspace& ws) const
  {
    ws.qd.resize(d_model);
    matvec(dec_attn_in_w.data(), dec_attn_in_b.data(), ws.q.data(), ws.qd.data(), d_model, d_model);
    ws.pk.swap(ws.kv_keys);
    ws.pv.swap(ws.kv_values);
    attention_core(ws.qd.data(), 1, mask, ws);
    ws.pk.swap(ws.kv_keys);
    ws.pv.swap(ws.kv_values);
    ws.g.resize(d_model);
    matvec(dec_attn_out_w.data(), dec_attn_out_b.data(), ws.ctx.data(), ws.g.data(), d_model, d_model);
  }

  void attention_core(const float* Q, unsigned nq, const uint8_t* mask, workspace& ws) const
  {
    const unsigned d  = d_model;
    const unsigned hd = d / n_heads;
    const unsigned k  = ws.k;
    const float    inv_scale = 1.f / std::sqrt(static_cast<float>(hd));

    ws.ctx.assign(static_cast<size_t>(nq) * d, 0.f);
    ws.w.resize(k);

    for (unsigned i = 0; i != nq; ++i) {
      for (unsigned head = 0; head != n_heads; ++head) {
        const unsigned off = head * hd;
        const float*   qrow = Q + static_cast<size_t>(i) * d + off;
        float          mx   = -std::numeric_limits<float>::infinity();
        for (unsigned j = 0; j != k; ++j) {
          if (mask != nullptr && mask[j] == 0) {
            ws.w[j] = -std::numeric_limits<float>::infinity();
            continue;
          }
          ws.w[j] = dot(qrow, ws.pk.data() + static_cast<size_t>(j) * d + off, hd) * inv_scale;
          mx      = ws.w[j] > mx ? ws.w[j] : mx;
        }
        if (!std::isfinite(mx)) {
          continue;
        }
        float sum = 0.f;
        for (unsigned j = 0; j != k; ++j) {
          ws.w[j] = std::isfinite(ws.w[j]) ? std::exp(ws.w[j] - mx) : 0.f;
          sum += ws.w[j];
        }
        if (sum <= 0.f) {
          continue;
        }
        const float inv_sum = 1.f / sum;
        float*      out     = ws.ctx.data() + static_cast<size_t>(i) * d + off;
        for (unsigned j = 0; j != k; ++j) {
          const float  a   = ws.w[j] * inv_sum;
          const float* row = ws.pv.data() + static_cast<size_t>(j) * d + off;
          for (unsigned c = 0; c != hd; ++c) {
            out[c] += a * row[c];
          }
        }
      }
    }
  }

  static void layer_norm(float* x, unsigned n, const std::vector<float>& gamma, const std::vector<float>& beta)
  {
    const unsigned d = static_cast<unsigned>(gamma.size());
    for (unsigned i = 0; i != n; ++i) {
      float* row  = x + static_cast<size_t>(i) * d;
      float  mean = 0.f;
      for (unsigned j = 0; j != d; ++j) {
        mean += row[j];
      }
      mean /= static_cast<float>(d);
      float var = 0.f;
      for (unsigned j = 0; j != d; ++j) {
        const float v = row[j] - mean;
        var += v * v;
      }
      var /= static_cast<float>(d);
      const float inv = 1.f / std::sqrt(var + 1e-5f);
      for (unsigned j = 0; j != d; ++j) {
        row[j] = (row[j] - mean) * inv * gamma[j] + beta[j];
      }
    }
  }

  static void masked_log_softmax(const uint8_t* mask, workspace& ws)
  {
    const float ninf = -std::numeric_limits<float>::infinity();
    const unsigned k = ws.k;
    ws.logp.assign(k, ninf);
    float mx = ninf;
    for (unsigned i = 0; i != k; ++i) {
      if (mask[i] != 0 && ws.logits[i] > mx) {
        mx = ws.logits[i];
      }
    }
    if (!std::isfinite(mx)) {
      return;
    }
    float sum = 0.f;
    for (unsigned i = 0; i != k; ++i) {
      if (mask[i] != 0) {
        sum += std::exp(ws.logits[i] - mx);
      }
    }
    const float lse = mx + std::log(sum);
    for (unsigned i = 0; i != k; ++i) {
      if (mask[i] != 0) {
        ws.logp[i] = ws.logits[i] - lse;
      }
    }
  }
};

inline std::shared_ptr<model> load_model_file(const std::string& path)
{
  FILE* fh = std::fopen(path.c_str(), "rb");
  if (fh == nullptr) {
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

  auto scalar = [&s](const char* key, double dflt) {
    const std::string k   = std::string("\"") + key + "\"";
    auto              pos = s.find(k);
    if (pos == std::string::npos) {
      return dflt;
    }
    pos = s.find(':', pos);
    return pos == std::string::npos ? dflt : std::atof(s.c_str() + pos + 1);
  };
  auto array = [&s](const char* key) {
    std::vector<float> out;
    const std::string  k   = std::string("\"") + key + "\"";
    auto               pos = s.find(k);
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
      char*        np = nullptr;
      const double v  = std::strtod(p, &np);
      if (np == p) {
        ++p;
        continue;
      }
      out.push_back(static_cast<float>(v));
      p = np;
      while (p < end && (*p == ',' || *p == ' ' || *p == '\n' || *p == '\t')) {
        ++p;
      }
    }
    return out;
  };

  auto text = [&s](const char* key) {
    const std::string k   = std::string("\"") + key + "\"";
    auto              pos = s.find(k);
    if (pos == std::string::npos) {
      return std::string{};
    }
    pos = s.find(':', pos);
    if (pos == std::string::npos) {
      return std::string{};
    }
    const auto lq = s.find('"', pos);
    const auto rq = lq == std::string::npos ? std::string::npos : s.find('"', lq + 1);
    return rq == std::string::npos ? std::string{} : s.substr(lq + 1, rq - lq - 1);
  };

  auto m     = std::make_shared<model>();
  m->n_feat  = static_cast<unsigned>(scalar("n_feat", 0.0));
  m->n_ctx   = static_cast<unsigned>(scalar("n_ctx", 0.0));
  m->d_model = static_cast<unsigned>(scalar("d_model", 0.0));
  m->n_heads = static_cast<unsigned>(scalar("n_heads", 0.0));

  m->role        = text("role");
  m->prb_group   = static_cast<unsigned>(scalar("prb_group", 0.0));
  m->cell_bw_mhz = scalar("cell_bw_mhz", 0.0);

  m->enc_proj_w     = array("enc_proj_w");
  m->enc_proj_b     = array("enc_proj_b");
  m->enc_attn_in_w  = array("enc_attn_in_w");
  m->enc_attn_in_b  = array("enc_attn_in_b");
  m->enc_attn_out_w = array("enc_attn_out_w");
  m->enc_attn_out_b = array("enc_attn_out_b");
  m->enc_norm1_w    = array("enc_norm1_w");
  m->enc_norm1_b    = array("enc_norm1_b");
  m->enc_ff1_w      = array("enc_ff1_w");
  m->enc_ff1_b      = array("enc_ff1_b");
  m->enc_ff2_w      = array("enc_ff2_w");
  m->enc_ff2_b      = array("enc_ff2_b");
  m->enc_norm2_w    = array("enc_norm2_w");
  m->enc_norm2_b    = array("enc_norm2_b");

  m->dec_ctx_w      = array("dec_ctx_w");
  m->dec_ctx_b      = array("dec_ctx_b");
  m->dec_attn_in_w  = array("dec_attn_in_w");
  m->dec_attn_in_b  = array("dec_attn_in_b");
  m->dec_attn_out_w = array("dec_attn_out_w");
  m->dec_attn_out_b = array("dec_attn_out_b");
  m->dec_delta_w    = array("dec_delta_w");
  m->dec_delta_b    = array("dec_delta_b");

  return m->valid() ? m : nullptr;
}

}
}
