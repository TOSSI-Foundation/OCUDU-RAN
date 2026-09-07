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
#include <cstdio>

int main(int argc, char** argv)
{
  if (argc < 2) {
    std::printf("usage: verify <model.model>\n");
    return 1;
  }
  auto m = ocudu::attn_sched::load_model_file(argv[1]);
  if (!m) {
    std::printf("LOAD_FAILED\n");
    return 1;
  }

  const unsigned     k = 12;
  std::vector<float> phi(static_cast<size_t>(k) * m->n_feat);
  for (unsigned i = 0; i != k; ++i) {
    for (unsigned f = 0; f != m->n_feat; ++f) {
      phi[static_cast<size_t>(i) * m->n_feat + f] =
          0.1f * static_cast<float>(i + 1) - 0.3f * static_cast<float>(f);
    }
  }
  std::vector<float> c_vec(m->n_ctx);
  for (unsigned i = 0; i != m->n_ctx; ++i) {
    c_vec[i] = 0.5f + 0.25f * static_cast<float>(i);
  }
  std::vector<float>   delta(k);
  std::vector<uint8_t> mask(k, 1);
  for (unsigned i = 0; i != k; ++i) {
    delta[i] = 1.0f - 0.05f * static_cast<float>(i);
    if (i % 5 == 3) {
      mask[i] = 0;
    }
  }

  ocudu::attn_sched::workspace ws;
  ws.reserve(m->d_model, k);
  m->encode(phi.data(), k, ws);
  m->prepare_decode(ws);

  std::vector<float>       h_prev(m->d_model, 0.f);
  const std::vector<float> logp = m->decode_step(h_prev.data(), c_vec.data(), delta.data(), mask.data(), ws);
  for (unsigned i = 0; i != k; ++i) {
    std::printf("%.9f%s", logp[i], i + 1 == k ? "\n" : ",");
  }

  const int best = ocudu::attn_sched::model::argmax(logp);
  if (best >= 0) {
    const float* row = ws.h_enc.data() + static_cast<size_t>(best) * m->d_model;
    std::copy(row, row + m->d_model, h_prev.begin());
    mask[best] = 0;
  }
  const std::vector<float> logp2 = m->decode_step(h_prev.data(), c_vec.data(), delta.data(), mask.data(), ws);
  for (unsigned i = 0; i != k; ++i) {
    std::printf("%.9f%s", logp2[i], i + 1 == k ? "\n" : ",");
  }
  return 0;
}
