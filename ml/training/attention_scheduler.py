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

import numpy as np
import torch
import torch.nn as nn


class Encoder(nn.Module):
    def __init__(self, n_feat, d_model, n_heads):
        super().__init__()
        self.proj = nn.Linear(n_feat, d_model)
        self.attn = nn.MultiheadAttention(d_model, n_heads, batch_first=True)
        self.norm1 = nn.LayerNorm(d_model)
        self.ff = nn.Sequential(nn.Linear(d_model, 4 * d_model), nn.ReLU(), nn.Linear(4 * d_model, d_model))
        self.norm2 = nn.LayerNorm(d_model)

    def forward(self, phi):
        h = self.proj(phi)
        a, _ = self.attn(h, h, h)
        h = self.norm1(h + a)
        return self.norm2(h + self.ff(h))


class Decoder(nn.Module):
    def __init__(self, d_model, n_ctx, n_heads):
        super().__init__()
        self.ctx = nn.Linear(2 * d_model + n_ctx, d_model)
        self.attn = nn.MultiheadAttention(d_model, n_heads, batch_first=True)
        self.delta = nn.Linear(1, d_model)
        self.scale = d_model ** 0.5

    def forward(self, h_enc, h_prev, c_vec, delta, mask):
        q = self.ctx(torch.cat([h_enc.mean(dim=1), h_prev, c_vec], dim=-1)).unsqueeze(1)
        g, _ = self.attn(q, h_enc, h_enc, key_padding_mask=~mask)
        keys = h_enc + self.delta(delta.unsqueeze(-1))
        logits = torch.bmm(keys, g.transpose(1, 2)).squeeze(-1) / self.scale
        return torch.log_softmax(logits.masked_fill(~mask, -torch.inf), dim=-1)


class AttentionScheduler(nn.Module):
    def __init__(self, n_feat, n_ctx, d_model=128, n_heads=8):
        super().__init__()
        self.n_feat, self.n_ctx, self.d_model, self.n_heads = n_feat, n_ctx, d_model, n_heads
        self.encoder = Encoder(n_feat, d_model, n_heads)
        self.decoder = Decoder(d_model, n_ctx, n_heads)

    def rollout(self, phi, env, greedy=False):
        h_enc = self.encoder(phi)
        b, k, d = h_enc.shape
        h_prev = torch.zeros(b, d, device=phi.device, dtype=phi.dtype)
        logp_total = torch.zeros(b, device=phi.device, dtype=phi.dtype)
        chosen = []

        for _ in range(k):
            mask = env.mask()
            if not mask.any():
                break
            alive = mask.any(dim=-1)
            safe_mask = mask.clone()
            safe_mask[~alive, 0] = True

            logp = self.decoder(h_enc, h_prev, env.context(), env.delta(), safe_mask)
            probs = logp.exp()
            idx = probs.argmax(dim=-1) if greedy else torch.multinomial(probs, 1).squeeze(-1)
            step_logp = logp.gather(1, idx.unsqueeze(1)).squeeze(1)
            logp_total = logp_total + torch.where(alive, step_logp, torch.zeros_like(step_logp))
            env.step(idx, alive)
            h_prev = torch.where(alive.unsqueeze(-1), h_enc[torch.arange(b), idx], h_prev)
            chosen.append(torch.where(alive, idx, torch.full_like(idx, -1)))

        return logp_total, torch.stack(chosen, dim=1) if chosen else torch.zeros(b, 0, dtype=torch.long)


def export_arrays(model):
    sd = {k: v.detach().cpu().numpy().astype(np.float64) for k, v in model.state_dict().items()}

    def flat(key):
        return sd[key].reshape(-1).tolist()

    out = {}
    out["enc_proj_w"], out["enc_proj_b"] = flat("encoder.proj.weight"), flat("encoder.proj.bias")
    out["enc_attn_in_w"] = flat("encoder.attn.in_proj_weight")
    out["enc_attn_in_b"] = flat("encoder.attn.in_proj_bias")
    out["enc_attn_out_w"] = flat("encoder.attn.out_proj.weight")
    out["enc_attn_out_b"] = flat("encoder.attn.out_proj.bias")
    out["enc_norm1_w"], out["enc_norm1_b"] = flat("encoder.norm1.weight"), flat("encoder.norm1.bias")
    out["enc_ff1_w"], out["enc_ff1_b"] = flat("encoder.ff.0.weight"), flat("encoder.ff.0.bias")
    out["enc_ff2_w"], out["enc_ff2_b"] = flat("encoder.ff.2.weight"), flat("encoder.ff.2.bias")
    out["enc_norm2_w"], out["enc_norm2_b"] = flat("encoder.norm2.weight"), flat("encoder.norm2.bias")
    out["dec_ctx_w"], out["dec_ctx_b"] = flat("decoder.ctx.weight"), flat("decoder.ctx.bias")
    out["dec_attn_in_w"] = flat("decoder.attn.in_proj_weight")
    out["dec_attn_in_b"] = flat("decoder.attn.in_proj_bias")
    out["dec_attn_out_w"] = flat("decoder.attn.out_proj.weight")
    out["dec_attn_out_b"] = flat("decoder.attn.out_proj.bias")
    out["dec_delta_w"], out["dec_delta_b"] = flat("decoder.delta.weight"), flat("decoder.delta.bias")
    return out
