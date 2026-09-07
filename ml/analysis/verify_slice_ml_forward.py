# Copyright 2025-2026 coRAN LABS Private Limited
#
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy of the License at
#
#     http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
# See the License for the specific language governing permissions and
# limitations under the License.

import json, sys, numpy as np, torch, torch.nn as nn
blob = json.load(open(sys.argv[1]))
N_IN, H, F, A = blob["n_in"], blob["d_hidden"], blob["d_fc"], blob["n_actions"]
mean = np.array(blob["feat_mean"]); scale = np.array(blob["feat_scale"])

lstm = nn.LSTM(N_IN, H, batch_first=True).double(); fc = nn.Linear(H, F).double(); out = nn.Linear(F, A).double()
def T(k, shape): return torch.tensor(np.array(blob[k]).reshape(shape), dtype=torch.float64)
with torch.no_grad():
    lstm.weight_ih_l0.copy_(torch.cat([T(f"w_{g}", (H, N_IN)) for g in "ifgo"]))
    lstm.weight_hh_l0.copy_(torch.cat([T(f"u_{g}", (H, H)) for g in "ifgo"]))
    lstm.bias_ih_l0.copy_(torch.cat([T(f"b_{g}", (H,)) for g in "ifgo"]))
    lstm.bias_hh_l0.zero_()
    fc.weight.copy_(T("w_fc", (F, H))); fc.bias.copy_(T("b_fc", (F,)))
    out.weight.copy_(T("w_out", (A, F))); out.bias.copy_(T("b_out", (A,)))

xs = []
for step in range(5):
    x = np.zeros(N_IN)
    x[0] = 40.0 + step * 7.0; x[1] = 1500.0 * (step + 1)
    x[2] = 0.9 - 0.05 * step; x[3] = 0.97 + 0.005 * step
    x[4 + (step % 3)] = 1.0
    xs.append((x - mean) / scale)
X = torch.tensor(np.array(xs), dtype=torch.float64).unsqueeze(0)
with torch.no_grad():
    h, _ = lstm(X)
    p = torch.softmax(out(torch.relu(fc(h)))[0], dim=1).numpy()
for row in p:
    print(",".join(f"{v:.9f}" for v in row))
