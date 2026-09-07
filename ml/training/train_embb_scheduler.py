#!/usr/bin/env python3
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

import argparse
import json
import os
import sys

import numpy as np
import torch

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from attention_scheduler import AttentionScheduler, export_arrays
import scheduler_env
from scheduler_env import NOISE_W, embb_sample

LAMBDA_EMBB_PEN = 100.0
R_MIN_BPS = 4e6

N_FEAT = 2
N_CTX = 1


class EmbbEnv:
    def __init__(self, batch):
        self.n_users, self.n_prbs = batch["n_users"], batch["n_prbs"]
        self.rate_pair = batch["rate_pair"]
        self.valid = batch["valid_pair"]
        self.zeta = batch["zeta"]
        b = self.rate_pair.shape[0]
        dev = self.rate_pair.device

        self.prb_taken = torch.zeros(b, self.n_prbs, dtype=torch.bool, device=dev)
        self.user_rate = torch.zeros(b, self.n_users, device=dev)
        self.user_rate_served = torch.zeros(b, self.n_users, device=dev)
        self.n_free = torch.full((b,), float(self.n_prbs), device=dev)

    def _prb_of(self, idx):
        return idx % self.n_prbs

    def delta(self):
        short = torch.clamp(R_MIN_BPS - self.user_rate, min=0.0) / R_MIN_BPS
        return short.repeat_interleave(self.n_prbs, dim=1)

    def context(self):
        return self.n_free.unsqueeze(-1)

    def mask(self):
        return (~self.prb_taken).repeat(1, self.n_users) & self.valid

    def step(self, idx, alive):
        b = idx.shape[0]
        rows = torch.arange(b, device=idx.device)
        u, k = idx // self.n_prbs, self._prb_of(idx)
        a = alive.to(self.user_rate.dtype)

        r = self.rate_pair[rows, idx]
        gained = torch.where(alive, r, torch.zeros_like(r))
        self.user_rate[rows, u] += gained
        self.user_rate_served[rows, u] += gained * self.zeta[rows, k]
        self.prb_taken[rows, k] |= alive
        self.n_free = self.n_free - a

    def objective(self):
        served = self.user_rate_served
        shortfall = torch.clamp(R_MIN_BPS - served, min=0.0) / R_MIN_BPS
        return served.sum(dim=1) / 1e6 - LAMBDA_EMBB_PEN * (shortfall ** 2).sum(dim=1)


def make_batch(rng, size, n_users, device, punct_rate):
    samples = [embb_sample(rng, n_users) for _ in range(size)]
    k_max = max(s["gains"].shape[1] for s in samples)

    gains = np.zeros((size, n_users, k_max))
    shared = np.zeros((size, k_max))
    valid = np.zeros((size, k_max), dtype=bool)
    bw = np.zeros(size)
    budget = np.zeros(size)

    for i, s in enumerate(samples):
        k = s["gains"].shape[1]
        gains[i, :, :k] = s["gains"]
        shared[i, :k] = s["shared"]
        valid[i, :k] = True
        bw[i] = s["prb_bw_hz"]
        budget[i] = s["power_budget"]

    n_valid = np.maximum(valid.sum(axis=1, keepdims=True), 1)
    p_per_prb = (budget[:, None] / n_valid)
    snr = p_per_prb[:, None, :] * gains / NOISE_W
    rate = bw[:, None, None] * np.log2(1.0 + np.maximum(snr, 0.0))
    rate = np.where(np.tile(valid[:, None, :], (1, n_users, 1)), rate, 0.0)

    surv = np.ones((size, k_max))
    sh = shared > 0.0
    surv[sh] = (rng.uniform(0.0, 1.0, int(sh.sum())) >= punct_rate).astype(np.float64)

    t = lambda a, dt=torch.float32: torch.tensor(a, dtype=dt, device=device)
    phi = np.stack(
        [
            np.log10(np.maximum(gains, 1e-30)).reshape(size, -1),
            np.tile(shared, (1, n_users)),
        ],
        axis=-1,
    )
    return {
        "phi": t(phi),
        "rate_pair": t(rate.reshape(size, -1)),
        "valid_pair": torch.tensor(np.tile(valid, (1, n_users)), dtype=torch.bool, device=device),
        "zeta": t(surv),
        "n_users": n_users,
        "n_prbs": k_max,
    }


def measure_puncturing(model_path, rng, device, n_samples=256):
    if not model_path:
        print("  WARNING: no --urllc-model given; using an ASSUMED puncturing rate of 0.5.")
        print("           Train Model 3 first and pass it, or the zeta this model learns from is invented.")
        return 0.5

    from train_urllc_scheduler import N_CTX as U_CTX
    from train_urllc_scheduler import N_FEAT as U_FEAT
    from train_urllc_scheduler import UrllcEnv
    from train_urllc_scheduler import make_batch as u_make_batch

    blob = json.load(open(model_path))
    um = AttentionScheduler(U_FEAT, U_CTX, blob["d_model"], blob["n_heads"]).to(device)
    sd = {}
    for k, v in um.state_dict().items():
        key = _EXPORT_KEY[k]
        sd[k] = torch.tensor(np.array(blob[key], dtype=np.float64).reshape(v.shape), dtype=v.dtype)
    um.load_state_dict(sd)
    um.eval()

    with torch.no_grad():
        b = u_make_batch(rng, n_samples, 4, 1.5, device)
        env = UrllcEnv(b)
        um.rollout(b["phi"], env, greedy=True)
        shared_mask = b["punct"][:, : b["n_prbs"]] > 0.0
        taken = env.prb_taken & shared_mask
        denom = shared_mask.sum().clamp_min(1)
        rate = float(taken.sum()) / float(denom)
    print(f"  measured puncturing rate from Model 3 : {rate:.4f}")
    return rate


_EXPORT_KEY = {
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


def rollout_objective(model, batch, greedy):
    env = EmbbEnv(batch)
    logp, _ = model.rollout(batch["phi"], env, greedy=greedy)
    return logp, env.objective()


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--model-out", default="ml/slicemanager/embb_scheduler.model")
    ap.add_argument("--urllc-model", default="", help="trained Model 3, used to measure zeta")
    ap.add_argument("--epochs", type=int, default=100, help="Algorithm 3: E_eMBB (paper: 100)")
    ap.add_argument("--batch", type=int, default=128)
    ap.add_argument("--steps-per-epoch", type=int, default=20)
    ap.add_argument("--d-model", type=int, default=128)
    ap.add_argument("--n-heads", type=int, default=8)
    ap.add_argument("--lr", type=float, default=1e-4)
    ap.add_argument("--users", type=int, default=6, help="eMBB users (paper Section V: 6)")
    ap.add_argument("--cell-bw-mhz", type=float, default=10.0,
                    help="total carrier bandwidth the model is trained for "
                         "(paper Section V: 10). Set to your deployment cell, e.g. 100")
    ap.add_argument("--prb-group", type=int, default=1,
                    help="contiguous PRBs per allocation unit; 1 = the paper's per-PRB decision. "
                         "Raise it to keep the unit count paper-sized on a wide carrier "
                         "(e.g. 16 for a 273-PRB cell)")
    ap.add_argument("--seed", type=int, default=0)
    ap.add_argument("--device", default="cpu")
    args = ap.parse_args()
    scheduler_env.set_cell_bandwidth_mhz(args.cell_bw_mhz)
    scheduler_env.set_prb_group(args.prb_group)

    torch.manual_seed(args.seed)
    rng = np.random.default_rng(args.seed)
    device = torch.device(args.device)

    print(f"eMBB scheduler (Model 4)  users={args.users}  R_min={R_MIN_BPS / 1e6:.0f} Mbps")
    punct_rate = measure_puncturing(args.urllc_model, rng, device)

    model = AttentionScheduler(N_FEAT, N_CTX, args.d_model, args.n_heads).to(device)
    opt = torch.optim.Adam(model.parameters(), lr=args.lr)

    for ep in range(args.epochs):
        total = 0.0
        for _ in range(args.steps_per_epoch):
            batch = make_batch(rng, args.batch, args.users, device, punct_rate)
            model.train()
            logp, obj = rollout_objective(model, batch, greedy=False)
            with torch.no_grad():
                model.eval()
                _, base = rollout_objective(model, batch, greedy=True)
            loss = (-(obj.detach() - base.detach()) * logp).mean()
            opt.zero_grad()
            loss.backward()
            opt.step()
            total += float(obj.mean())
        if (ep + 1) % max(1, args.epochs // 20) == 0 or ep == 0:
            print(f"  epoch {ep + 1:4d}/{args.epochs}  mean objective {total / args.steps_per_epoch:.4f}")

    model.eval()
    with torch.no_grad():
        batch = make_batch(rng, args.batch, args.users, device, punct_rate)
        _, greedy_obj = rollout_objective(model, batch, greedy=True)
    print(f"\n  held-out greedy objective : {float(greedy_obj.mean()):.4f}")

    blob = {
        "format": "ocudu_attention_scheduler",
        "version": 1,
        "role": "embb",
        "n_feat": N_FEAT,
        "n_ctx": N_CTX,
        "d_model": args.d_model,
        "n_heads": args.n_heads,
        **export_arrays(model),
        "meta": {
            "paper": "Setayesh/Bahrami/Wong IEEE TWC 2022, Model 4, Section IV-B2, Algorithm 3",
            "epochs": args.epochs,
            "users": args.users,
            "prb_group": args.prb_group,
            "cell_bw_mhz": args.cell_bw_mhz,
            "r_min_bps": R_MIN_BPS,
            "lambda_embb_pen": LAMBDA_EMBB_PEN,
            "puncturing_rate": punct_rate,
            "puncturing_measured": bool(args.urllc_model),
            "greedy_objective": float(greedy_obj.mean()),
        },
    }
    os.makedirs(os.path.dirname(os.path.abspath(args.model_out)), exist_ok=True)
    with open(args.model_out, "w") as fh:
        json.dump(blob, fh)
    print(f"  wrote {args.model_out}  ({os.path.getsize(args.model_out) / 1e6:.2f} MB)")


if __name__ == "__main__":
    main()
