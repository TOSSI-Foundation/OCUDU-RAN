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
import glob
import json
import os
import sys

import numpy as np
import torch
import torch.nn as nn

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from train_slicemanager_actor import (
    ACTION_MENU,
    N_ACTIONS,
    N_IN,
    N_OBS,
    load_slicemanager_rows,
)

GAMMA = 0.99
LR_CRITIC = 1e-4


class Critic(nn.Module):
    def __init__(self, d_hidden, d_fc):
        super().__init__()
        self.lstm = nn.LSTM(N_IN, d_hidden, batch_first=True)
        self.fc = nn.Linear(d_hidden, d_fc)
        self.out = nn.Linear(d_fc, 1)

    def forward(self, x):
        h, _ = self.lstm(x)
        return self.out(torch.relu(self.fc(h))).squeeze(-1)


def build_sequences(records, min_periods=50):
    runs = {}
    for r in records:
        if r["action"] is None:
            continue
        runs.setdefault(r["run"], []).append(r)

    seqs, dropped = [], []
    for run, rows in sorted(runs.items()):
        if len(rows) < min_periods:
            dropped.append((run.split("/")[-1], len(rows)))
            continue
        rows.sort(key=lambda r: r["period"])
        x = np.zeros((len(rows), N_IN), dtype=np.float32)
        rew = np.zeros(len(rows), dtype=np.float32)
        prev = rows[0]["action"]
        for i, r in enumerate(rows):
            x[i, :N_OBS] = r["obs"]
            x[i, N_OBS + prev] = 1.0
            rew[i] = r["utility"]
            prev = r["action"]
        seqs.append((x, rew))

    for name, n in dropped:
        print(f"  dropped {name}: {n} periods < --min-periods {min_periods}")
    return seqs


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("csv", nargs="+", help="slice_ml_slicemanager_*.csv files (globs allowed)")
    ap.add_argument("--model-out", default="ml/slicemanager/slicemanager_critic.model")
    ap.add_argument("--d-hidden", type=int, default=256, help="paper: 256")
    ap.add_argument("--d-fc", type=int, default=512, help="paper: 512")
    ap.add_argument("--epochs", type=int, default=200)
    ap.add_argument("--lr", type=float, default=LR_CRITIC)
    ap.add_argument("--gamma", type=float, default=GAMMA)
    ap.add_argument("--val-frac", type=float, default=0.25)
    ap.add_argument("--min-periods", type=int, default=50,
                    help="drop runs shorter than this; they cannot train or validate an LSTM")
    ap.add_argument("--seed", type=int, default=0)
    ap.add_argument("--device", default="cpu", help="cpu or cuda")
    ap.add_argument("--embb-sst", type=int, default=1)
    ap.add_argument("--urllc-sst", type=int, default=2)
    args = ap.parse_args()

    paths = []
    for p in args.csv:
        paths.extend(sorted(glob.glob(p)) or [p])
    paths = [p for p in paths if os.path.exists(p)]
    if not paths:
        sys.exit("ERROR: no input CSV found.")

    records = load_slicemanager_rows(paths, args.embb_sst, args.urllc_sst)
    seqs = build_sequences(records, args.min_periods)
    if len(seqs) < 2:
        sys.exit(
            f"ERROR: need at least 2 runs of >= {args.min_periods} periods to form a train/val split;\n"
            f"  got {len(seqs)}. The critic is fitted on transitions, so a single run cannot be validated."
        )

    torch.manual_seed(args.seed)
    dev = torch.device(args.device)
    allx = np.concatenate([x for x, _ in seqs], axis=0)
    mean = np.zeros(N_IN, dtype=np.float64)
    scale = np.ones(N_IN, dtype=np.float64)
    mean[:N_OBS] = allx[:, :N_OBS].mean(axis=0)
    s = allx[:, :N_OBS].std(axis=0)
    s[s == 0.0] = 1.0
    scale[:N_OBS] = s

    seqs.sort(key=lambda s: -len(s[1]))
    n_val = min(max(1, round(len(seqs) * args.val_frac)), len(seqs) - 1)
    val, train = seqs[1 : 1 + n_val], [seqs[0]] + seqs[1 + n_val :]
    print(f"  train runs: {[len(r) for _, r in train]} periods")
    print(f"  val runs  : {[len(r) for _, r in val]} periods")

    rew_mean = float(np.concatenate([r for _, r in train]).mean())
    print(f"  reward mean (centred out): {rew_mean:.4f}")

    model = Critic(args.d_hidden, args.d_fc).to(dev)
    opt = torch.optim.Adam(model.parameters(), lr=args.lr)

    def to_t(x):
        return torch.from_numpy(((x - mean) / scale).astype(np.float32)).unsqueeze(0).to(dev)

    def td_loss(x, rew):
        v = model(to_t(x))[0]
        r = torch.from_numpy(rew - rew_mean).to(dev)
        target = r[:-1] + args.gamma * v[1:].detach()
        return ((target - v[:-1]) ** 2).mean()

    print(f"SliceManager critic (Model 2)  runs={len(seqs)}  train={len(train)}  val={len(val)}  gamma={args.gamma}")
    best_vl, best_state, best_ep = float("inf"), None, 0
    for ep in range(args.epochs):
        model.train()
        tot = 0.0
        for x, rew in train:
            opt.zero_grad()
            loss = td_loss(x, rew)
            loss.backward()
            torch.nn.utils.clip_grad_norm_(model.parameters(), 1.0)
            opt.step()
            tot += loss.item()
        model.eval()
        with torch.no_grad():
            vl = float(np.mean([float(td_loss(x, r)) for x, r in val]))
        if vl < best_vl:
            best_vl, best_ep = vl, ep + 1
            best_state = {k: v.detach().clone() for k, v in model.state_dict().items()}
        if (ep + 1) % max(1, args.epochs // 10) == 0:
            print(f"  epoch {ep + 1:4d}/{args.epochs}  train TD {tot / len(train):.5f}  val TD {vl:.5f}")

    if best_state is not None:
        model.load_state_dict(best_state)
        print(f"  restored best epoch {best_ep} (val TD {best_vl:.5f})")

    model.eval()
    with torch.no_grad():
        val_td = float(np.mean([float(td_loss(x, r)) for x, r in val]))
        const_td = float(np.mean([np.mean((r[:-1] - rew_mean) ** 2) for _, r in val]))
    print(f"\n  val TD error         : {val_td:.5f}")
    print(f"  constant-value baseline : {const_td:.5f}")
    if not (val_td < const_td):
        print(
            "\n  WARNING: the critic does not beat a constant value estimate at its BEST epoch.\n"
            "  That is consistent with the observation not explaining the logged utility, but it is\n"
            "  NOT proof of it: offline TD on logged trajectories diverges here regardless (training\n"
            "  TD rises monotonically), so this bound is loose. The paper trains this on-policy\n"
            "  inside an environment (Section V, eq. 22); fitting it offline is a substitution the\n"
            "  paper does not make. Treat it as 'not fitted', not as 'the data is uninformative'."
        )

    sd = {k: v.detach().cpu().numpy().astype(np.float64) for k, v in model.state_dict().items()}
    H = args.d_hidden
    blob = {
        "format": "ocudu_slicemanager_critic",
        "version": 1,
        "n_in": N_IN,
        "d_hidden": H,
        "d_fc": args.d_fc,
        "feat_mean": mean.tolist(),
        "feat_scale": scale.tolist(),
        "meta": {
            "paper": "Setayesh/Bahrami/Wong IEEE TWC 2022, Model 2, eq. (8) and (23)",
            "gamma": args.gamma,
            "reward_mean": rew_mean,
            "runs": len(seqs),
            "val_td_error": val_td,
            "constant_baseline_td": const_td,
            "note": "Training-time only: the deployed actor is supervised, so no C++ runtime exists.",
        },
    }
    for gi, name in enumerate("ifgo"):
        blob[f"w_{name}"] = sd["lstm.weight_ih_l0"][gi * H : (gi + 1) * H].reshape(-1).tolist()
        blob[f"u_{name}"] = sd["lstm.weight_hh_l0"][gi * H : (gi + 1) * H].reshape(-1).tolist()
        blob[f"b_{name}"] = (
            sd["lstm.bias_ih_l0"][gi * H : (gi + 1) * H] + sd["lstm.bias_hh_l0"][gi * H : (gi + 1) * H]
        ).reshape(-1).tolist()
    blob["w_fc"], blob["b_fc"] = sd["fc.weight"].reshape(-1).tolist(), sd["fc.bias"].reshape(-1).tolist()
    blob["w_out"], blob["b_out"] = sd["out.weight"].reshape(-1).tolist(), sd["out.bias"].reshape(-1).tolist()

    os.makedirs(os.path.dirname(os.path.abspath(args.model_out)), exist_ok=True)
    with open(args.model_out, "w") as fh:
        json.dump(blob, fh)
    print(f"  wrote {args.model_out}  ({os.path.getsize(args.model_out) / 1e6:.2f} MB)")


if __name__ == "__main__":
    main()
