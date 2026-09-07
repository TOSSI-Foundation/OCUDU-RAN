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
import csv
import glob
import json
import os
import sys
from collections import defaultdict

import numpy as np

_LICENSE = """// SPDX-FileCopyrightText: Copyright (C) 2021-2026 Software Radio Systems Limited
// SPDX-License-Identifier: BSD-3-Clause-Open-MPI
// Portions of this file may implement 3GPP specifications, which may be subject to additional licensing requirements.
"""

ACTION_MENU = [
    (0, 12, 0, 0, 100, 0, 100),
    (6, 0, 6, 50, 50, 50, 50),
    (4, 4, 4, 33, 67, 33, 67),
    (8, 4, 0, 67, 100, 0, 33),
    (8, 0, 4, 67, 67, 33, 33),
    (4, 0, 8, 33, 33, 67, 67),
    (4, 8, 0, 33, 100, 0, 67),
    (0, 8, 4, 0, 67, 33, 100),
    (0, 4, 8, 0, 33, 67, 100),
    (8, 2, 2, 67, 83, 17, 33),
    (2, 8, 2, 17, 83, 17, 83),
    (2, 2, 8, 17, 33, 67, 83),
    (0, 0, 0, 70, 100, 30, 100),
    (0, 0, 0, 50, 100, 50, 100),
    (0, 0, 0, 60, 100, 40, 100),
    (0, 0, 0, 40, 100, 60, 100),
    (0, 0, 0, 80, 100, 20, 100),
    (0, 0, 0, 30, 100, 70, 100),
    (0, 0, 0, 90, 100, 10, 100),
]
N_ACTIONS = len(ACTION_MENU)

LAMBDA_SP = 0.01
LAMBDA_EMBB = 5.0
LAMBDA_URLLC = 5.0
UPSILON_EMBB = 0.90
UPSILON_URLLC = 0.99

OBS_FEATURES = ["embb_throughput_mbps", "urllc_demand_bytes", "embb_ssr", "urllc_ssr"]
N_OBS = len(OBS_FEATURES)
N_IN = N_OBS + N_ACTIONS


def utility(embb_mbps, embb_ssr, urllc_ssr):
    return (
        LAMBDA_SP * embb_mbps
        - LAMBDA_EMBB * max(0.0, UPSILON_EMBB - embb_ssr)
        - LAMBDA_URLLC * max(0.0, UPSILON_URLLC - urllc_ssr)
    )


def match_action(min_e, max_e, min_u, max_u, tol=3.0):
    best, best_err = None, None
    for idx, a in enumerate(ACTION_MENU):
        err = (
            abs(min_e - a[3]) + abs(max_e - a[4]) + abs(min_u - a[5]) + abs(max_u - a[6])
        )
        if best_err is None or err < best_err:
            best, best_err = idx, err
    if best_err is not None and best_err <= tol * 4:
        return best
    return None


def load_slicemanager_rows(paths, embb_sst=1, urllc_sst=2):
    periods = defaultdict(dict)
    for path in paths:
        with open(path, newline="") as fh:
            for row in csv.DictReader(fh):
                try:
                    key = (path, int(row["period_idx"]))
                    sst = int(row["sst"])
                except (KeyError, ValueError):
                    continue
                periods[key][sst] = row

    records = []
    for (path, pidx), slices in sorted(periods.items()):
        e, u = slices.get(embb_sst), slices.get(urllc_sst)
        if e is None or u is None:
            continue

        def num(row, col):
            v = row.get(col, "")
            if v is None or v == "":
                return None
            try:
                return float(v)
            except ValueError:
                return None

        embb_ssr = num(e, "ssr_embb")
        urllc_ssr = num(u, "ssr_urllc")
        if embb_ssr is None or urllc_ssr is None:
            continue

        dl_bs = num(u, "dl_bs_sum") or 0.0
        bsr = num(u, "bsr_sum") or 0.0
        embb_mbps = (num(e, "dl_brate_kbps_sum") or 0.0) / 1000.0

        action = match_action(
            num(e, "min_prb_ratio") or 0.0,
            num(e, "max_prb_ratio") or 0.0,
            num(u, "min_prb_ratio") or 0.0,
            num(u, "max_prb_ratio") or 0.0,
        )

        records.append(
            {
                "run": path,
                "period": pidx,
                "scenario": e.get("scenario", "default"),
                "obs": [embb_mbps, dl_bs + bsr, embb_ssr, urllc_ssr],
                "action": action,
                "utility": utility(embb_mbps, embb_ssr, urllc_ssr),
            }
        )
    return records


def bucket_of(rec, n_bins=4):
    demand, embb = rec["obs"][1], rec["obs"][0]
    d_bin = min(int(np.log10(demand + 1.0)), n_bins - 1)
    e_bin = min(int(embb // 25.0), n_bins - 1)
    return (rec["scenario"], d_bin, e_bin)


def build_supervision(records):
    by_bucket = defaultdict(lambda: defaultdict(list))
    for r in records:
        if r["action"] is not None:
            by_bucket[bucket_of(r)][r["action"]].append(r["utility"])

    best_per_bucket = {}
    for b, per_action in by_bucket.items():
        means = {a: float(np.mean(v)) for a, v in per_action.items()}
        best_per_bucket[b] = max(means, key=means.get)

    samples = []
    prev_action = {}
    for r in records:
        if r["action"] is None:
            continue
        b = bucket_of(r)
        if b not in best_per_bucket:
            continue
        pa = prev_action.get(r["run"], r["action"])
        samples.append({"run": r["run"], "obs": r["obs"], "prev_action": pa, "target": best_per_bucket[b]})
        prev_action[r["run"]] = r["action"]
    return samples, best_per_bucket


def assert_trainable(records, samples):
    actions = sorted({r["action"] for r in records if r["action"] is not None})
    unmatched = sum(1 for r in records if r["action"] is None)

    print(f"  periods loaded      : {len(records)}")
    print(f"  periods on the menu : {len(records) - unmatched}  (off-menu: {unmatched})")
    print(f"  distinct actions    : {len(actions)}  {actions}")

    if unmatched == len(records):
        sys.exit(
            "\nERROR: no period matched an action on the menu.\n"
            "  The PRB ratios in this capture are not one of the 12 paper tuples. Either run the\n"
            "  sweep with the menu's splits, or extend ACTION_MENU (and SLICE_ML_ACTION_MENU in\n"
            "  slice_ml_controller.h, in the same order) to cover the splits you actually used."
        )
    if len(actions) < 2:
        sys.exit(
            f"\nERROR: the dataset contains only {len(actions)} distinct action(s): {actions}.\n"
            "  An action-selection policy cannot be learned from data in which the action never\n"
            "  varies -- every label would be the same regardless of the observation, and the\n"
            "  resulting model would encode nothing but that constant.\n\n"
            "  Run the config sweep first (ml/SLICE_ML_PLAN.md Phase 3): the same traffic scenario\n"
            "  repeated across several PRB splits, so that utility can be compared between them."
        )
    targets = sorted({s["target"] for s in samples})
    if len(targets) < 2:
        sys.exit(
            f"\nERROR: every traffic bucket resolved to the same best action ({targets}).\n"
            "  The sweep covered multiple actions, but one dominates everywhere, so there is no\n"
            "  decision to learn -- a constant policy is the correct answer and does not need a\n"
            "  network. Widen the scenario matrix (especially the crossover band where the best\n"
            "  split flips) before training."
        )


def train(samples, d_hidden, d_fc, epochs, lr, seed, val_frac, device="cpu", min_periods=50,
          shuffle_prev_action=False):
    import torch
    import torch.nn as nn

    torch.manual_seed(seed)
    rng = np.random.default_rng(seed)
    dev = torch.device(device)

    runs = defaultdict(list)
    for s in samples:
        runs[s["run"]].append(s)

    seqs = []
    dropped = []
    for run, rows in sorted(runs.items()):
        if len(rows) < min_periods:
            dropped.append((run.split("/")[-1], len(rows)))
            continue
        x = np.zeros((len(rows), N_IN), dtype=np.float32)
        y = np.zeros((len(rows),), dtype=np.int64)
        for i, r in enumerate(rows):
            x[i, :N_OBS] = r["obs"]
            pa = int(rng.integers(N_ACTIONS)) if shuffle_prev_action else r["prev_action"]
            x[i, N_OBS + pa] = 1.0
            y[i] = r["target"]
        seqs.append((x, y))

    for name, n in dropped:
        print(f"  dropped {name}: {n} periods < --min-periods {min_periods}")
    if len(seqs) < 2:
        sys.exit(
            f"\nERROR: only {len(seqs)} run(s) of >= {min_periods} periods; need at least 2 to form a\n"
            "  train/validation split. Capture longer runs, or lower --min-periods knowing that a\n"
            "  short validation run makes the reported accuracy meaningless."
        )

    allx = np.concatenate([x for x, _ in seqs], axis=0)
    mean = np.zeros(N_IN, dtype=np.float64)
    scale = np.ones(N_IN, dtype=np.float64)
    mean[:N_OBS] = allx[:, :N_OBS].mean(axis=0)
    s = allx[:, :N_OBS].std(axis=0)
    s[s == 0.0] = 1.0
    scale[:N_OBS] = s

    seqs.sort(key=lambda s: -len(s[1]))
    n_val = max(1, round(len(seqs) * val_frac))
    n_val = min(n_val, len(seqs) - 1)
    val_seqs, train_seqs = seqs[1 : 1 + n_val], [seqs[0]] + seqs[1 + n_val :]
    print(f"  train runs: {[len(y) for _, y in train_seqs]} periods")
    print(f"  val runs  : {[len(y) for _, y in val_seqs]} periods")

    class Actor(nn.Module):
        def __init__(self):
            super().__init__()
            self.lstm = nn.LSTM(N_IN, d_hidden, batch_first=True)
            self.fc = nn.Linear(d_hidden, d_fc)
            self.out = nn.Linear(d_fc, N_ACTIONS)

        def forward(self, x):
            h, _ = self.lstm(x)
            return self.out(torch.relu(self.fc(h)))

    model = Actor().to(dev)
    opt = torch.optim.Adam(model.parameters(), lr=lr)
    lossf = nn.CrossEntropyLoss()

    def to_tensor(x):
        return torch.from_numpy(((x - mean) / scale).astype(np.float32)).unsqueeze(0).to(dev)

    def to_target(y):
        return torch.from_numpy(y).to(dev)

    import copy

    def val_accuracy():
        model.eval()
        correct = total = 0
        with torch.no_grad():
            for x, y in val_seqs:
                pred = model(to_tensor(x))[0].argmax(dim=1).cpu().numpy()
                correct += int((pred == y).sum())
                total += len(y)
        return correct / total if total else float("nan")

    best_acc, best_state, best_ep = -1.0, None, 0

    for ep in range(epochs):
        model.train()
        tot = 0.0
        for x, y in train_seqs:
            opt.zero_grad()
            logits = model(to_tensor(x))[0]
            loss = lossf(logits, to_target(y))
            loss.backward()
            torch.nn.utils.clip_grad_norm_(model.parameters(), 1.0)
            opt.step()
            tot += loss.item()
        if val_seqs:
            acc = val_accuracy()
            if acc > best_acc:
                best_acc, best_ep = acc, ep + 1
                best_state = copy.deepcopy(model.state_dict())
            if (ep + 1) % max(1, epochs // 10) == 0:
                print(f"  epoch {ep + 1:4d}  train_loss {tot / len(train_seqs):.4f}  val_acc {acc:.3f}")

    if best_state is not None:
        model.load_state_dict(best_state)
        print(f"  restored best epoch {best_ep} (val_acc {best_acc:.3f})")

    val_acc = majority_acc = float("nan")
    if val_seqs:
        model.eval()
        correct = total = 0
        counts = np.zeros(N_ACTIONS, dtype=np.int64)
        with torch.no_grad():
            for x, y in val_seqs:
                pred = model(to_tensor(x))[0].argmax(dim=1).cpu().numpy()
                correct += int((pred == y).sum())
                total += len(y)
                for t in y:
                    counts[t] += 1
        val_acc = correct / total if total else float("nan")
        majority_acc = counts.max() / counts.sum() if counts.sum() else float("nan")

    return model, mean, scale, val_acc, majority_acc


def extract_weights(model, d_hidden, d_fc):
    sd = {k: v.detach().cpu().numpy().astype(np.float64) for k, v in model.state_dict().items()}
    w_ih = sd["lstm.weight_ih_l0"]
    w_hh = sd["lstm.weight_hh_l0"]
    b_ih = sd["lstm.bias_ih_l0"]
    b_hh = sd["lstm.bias_hh_l0"]
    H = d_hidden

    out = {}
    for gi, name in enumerate(["i", "f", "g", "o"]):
        sl = slice(gi * H, (gi + 1) * H)
        out[f"w_{name}"] = w_ih[sl].reshape(-1).tolist()
        out[f"u_{name}"] = w_hh[sl].reshape(-1).tolist()
        out[f"b_{name}"] = (b_ih[sl] + b_hh[sl]).reshape(-1).tolist()

    out["w_fc"] = sd["fc.weight"].reshape(-1).tolist()
    out["b_fc"] = sd["fc.bias"].reshape(-1).tolist()
    out["w_out"] = sd["out.weight"].reshape(-1).tolist()
    out["b_out"] = sd["out.bias"].reshape(-1).tolist()
    return out


def export_model_file(arrays, mean, scale, d_hidden, d_fc, path, meta):
    blob = {
        "format": "ocudu_slice_ml_actor",
        "version": 1,
        "n_in": int(N_IN),
        "d_hidden": int(d_hidden),
        "d_fc": int(d_fc),
        "n_actions": int(N_ACTIONS),
        "feat_mean": [float(v) for v in mean],
        "feat_scale": [float(v) for v in scale],
        **arrays,
        "meta": meta,
    }
    os.makedirs(os.path.dirname(os.path.abspath(path)), exist_ok=True)
    with open(path, "w") as fh:
        json.dump(blob, fh)
    print(f"  wrote model file    : {path}  ({os.path.getsize(path) / 1e6:.2f} MB)")


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("csv", nargs="+", help="slice_ml_slicemanager_*.csv files (globs allowed)")
    ap.add_argument("--model-out", default="ml/slicemanager/slicemanager_actor.model")
    ap.add_argument("--d-hidden", type=int, default=256, help="LSTM hidden units (paper: 256)")
    ap.add_argument("--d-fc", type=int, default=512, help="FC width (paper: 512)")
    ap.add_argument("--epochs", type=int, default=200)
    ap.add_argument("--lr", type=float, default=1e-3)
    ap.add_argument("--seed", type=int, default=0)
    ap.add_argument("--device", default="cpu", help="cpu or cuda")
    ap.add_argument("--val-frac", type=float, default=0.25, help="fraction of RUNS held out")
    ap.add_argument("--min-periods", type=int, default=50,
                    help="drop runs shorter than this; they cannot train or validate an LSTM")
    ap.add_argument("--shuffle-prev-action", action="store_true",
                    help="Randomize the prev-action one-hot during training. Use when the captures hold "
                         "the split fixed per run: there prev_action==target in 83%% of samples, so the "
                         "network learns to echo it and is INERT in deployment (never switches). "
                         "Randomizing forces the policy to be a function of the observation. Expect a "
                         "LOWER held-out accuracy that is nonetheless real signal above the majority class.")
    ap.add_argument("--embb-sst", type=int, default=1)
    ap.add_argument("--urllc-sst", type=int, default=2)
    args = ap.parse_args()

    paths = []
    for p in args.csv:
        paths.extend(sorted(glob.glob(p)) or [p])
    paths = [p for p in paths if os.path.exists(p)]
    if not paths:
        sys.exit("ERROR: no input CSV found.")

    print(f"Slice-ML actor training\n  input files         : {len(paths)}")
    records = load_slicemanager_rows(paths, args.embb_sst, args.urllc_sst)
    if not records:
        sys.exit("ERROR: no usable periods (need both slices present with computable SSRs).")

    samples, best_per_bucket = build_supervision(records)
    assert_trainable(records, samples)

    print(f"  training samples    : {len(samples)}")
    print(f"  traffic buckets     : {len(best_per_bucket)}")

    try:
        import torch
    except ImportError:
        sys.exit("ERROR: PyTorch is required to train. Install it, or use --help to see the options.")

    model, mean, scale, val_acc, majority_acc = train(
        samples, args.d_hidden, args.d_fc, args.epochs, args.lr, args.seed, args.val_frac, args.device,
        args.min_periods, args.shuffle_prev_action
    )

    print(f"\n  held-out best-action accuracy : {val_acc:.3f}")
    print(f"  majority-class baseline       : {majority_acc:.3f}")
    if not (val_acc > majority_acc):
        print(
            "\n  WARNING: the model does not beat the majority-class baseline on held-out runs.\n"
            "  It is being exported so the numbers can be inspected, but it should not be promoted\n"
            "  to inference_apply -- a constant policy is doing at least as well."
        )

    arrays = extract_weights(model, args.d_hidden, args.d_fc)
    meta = {
        "paper": "Setayesh/Bahrami/Wong, IEEE TWC 2022, actor network (Section V, Fig. 4)",
        "n_runs": len({s["run"] for s in samples}),
        "n_samples": len(samples),
        "val_best_action_acc": float(val_acc),
        "majority_baseline_acc": float(majority_acc),
        "action_menu": ACTION_MENU,
        "obs_features": OBS_FEATURES,
        "reward_weights": {
            "lambda_sp": LAMBDA_SP,
            "lambda_embb": LAMBDA_EMBB,
            "lambda_urllc": LAMBDA_URLLC,
            "upsilon_embb": UPSILON_EMBB,
            "upsilon_urllc": UPSILON_URLLC,
        },
    }
    export_model_file(arrays, mean, scale, args.d_hidden, args.d_fc, args.model_out, meta)
    print("\nDone. Point slice_ml.inference.model_path at the exported file to load it at run time.")


if __name__ == "__main__":
    main()
