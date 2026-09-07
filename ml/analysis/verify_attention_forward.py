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

import json
import sys

import numpy as np
import torch

sys.path.insert(0, "ml/training")
from attention_scheduler import AttentionScheduler

blob = json.load(open(sys.argv[1]))
model = AttentionScheduler(blob["n_feat"], blob["n_ctx"], blob["d_model"], blob["n_heads"]).double()

KEY = {
    "encoder.proj.weight": "enc_proj_w", "encoder.proj.bias": "enc_proj_b",
    "encoder.attn.in_proj_weight": "enc_attn_in_w", "encoder.attn.in_proj_bias": "enc_attn_in_b",
    "encoder.attn.out_proj.weight": "enc_attn_out_w", "encoder.attn.out_proj.bias": "enc_attn_out_b",
    "encoder.norm1.weight": "enc_norm1_w", "encoder.norm1.bias": "enc_norm1_b",
    "encoder.ff.0.weight": "enc_ff1_w", "encoder.ff.0.bias": "enc_ff1_b",
    "encoder.ff.2.weight": "enc_ff2_w", "encoder.ff.2.bias": "enc_ff2_b",
    "encoder.norm2.weight": "enc_norm2_w", "encoder.norm2.bias": "enc_norm2_b",
    "decoder.ctx.weight": "dec_ctx_w", "decoder.ctx.bias": "dec_ctx_b",
    "decoder.attn.in_proj_weight": "dec_attn_in_w", "decoder.attn.in_proj_bias": "dec_attn_in_b",
    "decoder.attn.out_proj.weight": "dec_attn_out_w", "decoder.attn.out_proj.bias": "dec_attn_out_b",
    "decoder.delta.weight": "dec_delta_w", "decoder.delta.bias": "dec_delta_b",
}
sd = {k: torch.tensor(np.array(blob[KEY[k]]).reshape(v.shape), dtype=torch.float64)
      for k, v in model.state_dict().items()}
model.load_state_dict(sd)
model.eval()

K = 12
n_feat, n_ctx, d = blob["n_feat"], blob["n_ctx"], blob["d_model"]
phi = np.array([[0.1 * (i + 1) - 0.3 * f for f in range(n_feat)] for i in range(K)])
c_vec = np.array([[0.5 + 0.25 * i for i in range(n_ctx)]])
delta = np.array([[1.0 - 0.05 * i for i in range(K)]])
mask = np.array([[(i % 5) != 3 for i in range(K)]])

with torch.no_grad():
    h_enc = model.encoder(torch.tensor(phi, dtype=torch.float64).unsqueeze(0))
    h_prev = torch.zeros(1, d, dtype=torch.float64)
    args = (h_enc, h_prev, torch.tensor(c_vec), torch.tensor(delta), torch.tensor(mask))
    logp = model.decoder(*args)[0].numpy()
    print(",".join(f"{v:.9f}" for v in logp))

    finite = np.where(np.isfinite(logp))[0]
    if len(finite):
        best = int(finite[np.argmax(logp[finite])])
        h_prev = h_enc[0, best].unsqueeze(0)
        mask[0, best] = False
    logp2 = model.decoder(h_enc, h_prev, torch.tensor(c_vec), torch.tensor(delta),
                          torch.tensor(mask))[0].numpy()
    print(",".join(f"{v:.9f}" for v in logp2))
