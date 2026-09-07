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

#include "lib/scheduler/support/slice_ml_predictor.h"
#include <cstdio>
int main(int argc, char** argv)
{
  auto m = ocudu::slice_ml::load_actor_model_file(argv[1]);
  if (!m) { std::printf("LOAD_FAILED\n"); return 1; }
  std::vector<double> h(m->d_hidden, 0.0), c(m->d_hidden, 0.0);
  for (int step = 0; step < 5; ++step) {
    std::vector<double> x(m->n_in, 0.0);
    x[0] = 40.0 + step * 7.0;  x[1] = 1500.0 * (step + 1);
    x[2] = 0.9 - 0.05 * step;  x[3] = 0.97 + 0.005 * step;
    x[4 + (step % 3)] = 1.0;
    m->lstm_step(x, h, c);
    auto p = ocudu::slice_ml::actor_model::softmax(m->logits_from_hidden(h));
    for (size_t i = 0; i != p.size(); ++i) std::printf("%.9f%s", p[i], i + 1 == p.size() ? "\n" : ",");
  }
  return 0;
}
