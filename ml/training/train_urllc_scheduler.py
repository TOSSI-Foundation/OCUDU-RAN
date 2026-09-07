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
from scheduler_env import urllc_pair_power, urllc_rate_bits_torch, urllc_sample

LAMBDA_PUNC = 2.0
LAMBDA_URLLC_PEN = 10.0

N_FEAT = 3
N_CTX = 2


class UrllcEnv:
    def __init__(self, batch):
        self.n_users, self.n_prbs = batch["n_users"], batch["n_prbs"]
        self.pair_power = batch["pair_power"]
        self.blocklength = batch["blocklength"]
        self.demand0 = batch["demand"]
        b = self.pair_power.shape[0]
        dev = self.pair_power.device

        self.remaining_power = batch["power_budget"].clone()
        self.prb_taken = torch.zeros(b, self.n_prbs, dtype=torch.bool, device=dev)
        self.remaining = self.demand0.clone()
        self.alloc_count = torch.zeros(b, self.n_users, device=dev)
        self.n_punctured = torch.zeros(b, device=dev)
        self.power_used = torch.zeros(b, device=dev)
        self.punct = batch["punct"]

    def _user_of(self, idx):
        return idx // self.n_prbs

    def _prb_of(self, idx):
        return idx % self.n_prbs

    def delta(self):
        return self.remaining.repeat_interleave(self.n_prbs, dim=1)

    def context(self):
        free = (~self.prb_taken).sum(dim=1).to(self.remaining_power.dtype)
        return torch.stack([self.remaining_power, free], dim=-1)

    def mask(self):
        free = ~self.prb_taken.repeat(1, self.n_users)
        affordable = self.pair_power <= self.remaining_power.unsqueeze(-1)
        wanted = self.delta() > 0.0
        return free & affordable & wanted

    def step(self, idx, alive):
        b = idx.shape[0]
        rows = torch.arange(b, device=idx.device)
        u, k = self._user_of(idx), self._prb_of(idx)
        a = alive.to(self.remaining_power.dtype)

        raw_p = self.pair_power[rows, idx]
        p = torch.where(alive, raw_p, torch.zeros_like(raw_p))
        self.remaining_power = self.remaining_power - p
        self.power_used = self.power_used + p
        self.n_punctured = self.n_punctured + self.punct[rows, k] * a
        self.prb_taken[rows, k] |= alive
        self.alloc_count[rows, u] += a

        served = urllc_rate_bits_torch(self.alloc_count[rows, u], self.blocklength)
        new_rem = torch.clamp(self.demand0[rows, u] - served, min=0.0)
        self.remaining[rows, u] = torch.where(alive, new_rem, self.remaining[rows, u])

    def cost(self):
        unmet = (self.remaining > 0.0).sum(dim=1).to(self.power_used.dtype)
        return self.power_used + LAMBDA_PUNC * self.n_punctured + LAMBDA_URLLC_PEN * unmet


def make_batch(rng, size, n_users, arrival, device):
    samples = [urllc_sample(rng, n_users, arrival) for _ in range(size)]
    k_max = max(s["gains"].shape[1] for s in samples)

    gains = np.zeros((size, n_users, k_max))
    punct = np.zeros((size, k_max))
    valid = np.zeros((size, k_max), dtype=bool)
    demand = np.zeros((size, n_users))
    budget = np.zeros(size)
    blocklen = np.zeros(size)

    for i, s in enumerate(samples):
        k = s["gains"].shape[1]
        gains[i, :, :k] = s["gains"]
        punct[i, :k] = s["punct"]
        valid[i, :k] = True
        demand[i] = s["demand"]
        budget[i] = s["power_budget"]
        blocklen[i] = s["blocklength"]

    pair_power = np.where(gains > 0.0, urllc_pair_power(np.maximum(gains, 1e-30)), np.inf)
    pair_power = pair_power.reshape(size, n_users * k_max)
    pair_power[~np.tile(valid, (1, n_users))] = np.inf

    t = lambda a, dt=torch.float32: torch.tensor(a, dtype=dt, device=device)
    phi = np.stack(
        [
            np.log10(np.maximum(gains, 1e-30)).reshape(size, -1),
            np.repeat(demand, k_max, axis=1) / 1e3,
            np.tile(punct, (1, n_users)),
        ],
        axis=-1,
    )
    return {
        "phi": t(phi),
        "pair_power": t(pair_power),
        "punct": t(np.tile(punct, (1, n_users))),
        "demand": t(demand),
        "power_budget": t(budget),
        "blocklength": t(blocklen),
        "n_users": n_users,
        "n_prbs": k_max,
    }


def rollout_cost(model, batch, greedy):
    env = UrllcEnv(batch)
    logp, _ = model.rollout(batch["phi"], env, greedy=greedy)
    return logp, env.cost()


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--model-out", default="ml/slicemanager/urllc_scheduler.model")
    ap.add_argument("--epochs", type=int, default=100, help="Algorithm 2: E_URLLC (paper: 100)")
    ap.add_argument("--batch", type=int, default=512, help="samples per step (paper kappa: 1.28e6 total)")
    ap.add_argument("--steps-per-epoch", type=int, default=20)
    ap.add_argument("--d-model", type=int, default=128)
    ap.add_argument("--n-heads", type=int, default=8)
    ap.add_argument("--lr", type=float, default=1e-4)
    ap.add_argument("--users", type=int, default=4, help="URLLC users (paper Section V: 4)")
    ap.add_argument("--arrival", type=float, default=1.5, help="packets/ms per user (paper: 1-4)")
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

    model = AttentionScheduler(N_FEAT, N_CTX, args.d_model, args.n_heads).to(device)
    opt = torch.optim.Adam(model.parameters(), lr=args.lr)

    print(f"URLLC scheduler (Model 3)  users={args.users}  arrival={args.arrival} pkt/ms")
    for ep in range(args.epochs):
        total = 0.0
        for _ in range(args.steps_per_epoch):
            batch = make_batch(rng, args.batch, args.users, args.arrival, device)
            model.train()
            logp, cost = rollout_cost(model, batch, greedy=False)
            with torch.no_grad():
                model.eval()
                _, base = rollout_cost(model, batch, greedy=True)
            loss = ((cost.detach() - base.detach()) * logp).mean()
            opt.zero_grad()
            loss.backward()
            opt.step()
            total += float(cost.mean())
        if (ep + 1) % max(1, args.epochs // 20) == 0 or ep == 0:
            print(f"  epoch {ep + 1:4d}/{args.epochs}  mean cost {total / args.steps_per_epoch:.4f}")

    model.eval()
    with torch.no_grad():
        batch = make_batch(rng, args.batch, args.users, args.arrival, device)
        _, greedy_cost = rollout_cost(model, batch, greedy=True)
    print(f"\n  held-out greedy cost : {float(greedy_cost.mean()):.4f}")

    blob = {
        "format": "ocudu_attention_scheduler",
        "version": 1,
        "role": "urllc",
        "n_feat": N_FEAT,
        "n_ctx": N_CTX,
        "d_model": args.d_model,
        "n_heads": args.n_heads,
        **export_arrays(model),
        "meta": {
            "paper": "Setayesh/Bahrami/Wong IEEE TWC 2022, Model 3, Section IV-B1, Algorithm 2",
            "epochs": args.epochs,
            "users": args.users,
            "prb_group": args.prb_group,
            "cell_bw_mhz": args.cell_bw_mhz,
            "arrival_pkt_per_ms": args.arrival,
            "lambda_punc": LAMBDA_PUNC,
            "lambda_urllc_pen": LAMBDA_URLLC_PEN,
            "greedy_cost": float(greedy_cost.mean()),
        },
    }
    os.makedirs(os.path.dirname(os.path.abspath(args.model_out)), exist_ok=True)
    with open(args.model_out, "w") as fh:
        json.dump(blob, fh)
    print(f"  wrote {args.model_out}  ({os.path.getsize(args.model_out) / 1e6:.2f} MB)")


if __name__ == "__main__":
    main()
