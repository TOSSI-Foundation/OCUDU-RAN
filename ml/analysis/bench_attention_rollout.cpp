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

#include "lib/scheduler/support/attention_scheduler_predictor.h"
#include <chrono>
#include <cstdio>
#include <cstdlib>

using namespace ocudu::attn_sched;
using clk = std::chrono::steady_clock;

static double us_since(clk::time_point t0)
{
  return std::chrono::duration_cast<std::chrono::nanoseconds>(clk::now() - t0).count() / 1000.0;
}

int main(int argc, char** argv)
{
  if (argc < 2) {
    std::printf("usage: bench <model.model> [reps]\n");
    return 1;
  }
  auto m = load_model_file(argv[1]);
  if (!m) {
    std::printf("LOAD_FAILED\n");
    return 1;
  }
  const unsigned reps = argc > 2 ? static_cast<unsigned>(std::atoi(argv[2])) : 200;

  std::printf("model role=%s d_model=%u n_heads=%u n_feat=%u n_ctx=%u\n\n",
              m->role.c_str(), m->d_model, m->n_heads, m->n_feat, m->n_ctx);
  std::printf("%5s %6s %6s %7s %10s %10s %10s\n", "n_ue", "units", "K", "steps", "enc_us", "dec_us", "total_us");

  const unsigned ue_set[]    = {1, 2, 4};
  const unsigned units_set[] = {8, 17, 34};

  workspace ws;
  ws.reserve(m->d_model, 4 * 34);

  for (unsigned n_ues : ue_set) {
    for (unsigned n_units : units_set) {
      const unsigned k = n_ues * n_units;

      std::vector<float> phi(static_cast<size_t>(k) * m->n_feat);
      for (size_t i = 0; i != phi.size(); ++i) {
        phi[i] = 0.05f * static_cast<float>(i % 37) - 0.4f;
      }
      std::vector<float>   c_vec(m->n_ctx, 0.5f), delta(k, 1.0f), h_prev(m->d_model, 0.f);
      std::vector<uint8_t> mask(k, 1);

      double         enc_us = 0.0, dec_us = 0.0;
      volatile float sink = 0.f;

      for (unsigned r = 0; r != reps; ++r) {
        auto t0 = clk::now();
        m->encode(phi.data(), k, ws);
        m->prepare_decode(ws);
        enc_us += us_since(t0);

        auto t1 = clk::now();
        for (unsigned s = 0; s != n_units; ++s) {
          const std::vector<float>& lp = m->decode_step(h_prev.data(), c_vec.data(), delta.data(), mask.data(), ws);
          sink += lp[0];
        }
        dec_us += us_since(t1);
      }
      (void)sink;
      const double e = enc_us / reps, dc = dec_us / reps;
      std::printf("%5u %6u %6u %7u %10.1f %10.1f %10.1f\n", n_ues, n_units, k, n_units, e, dc, e + dc);
    }
  }
  return 0;
}
