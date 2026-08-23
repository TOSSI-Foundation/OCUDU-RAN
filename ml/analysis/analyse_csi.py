#!/usr/bin/env python3
import argparse
import csv
import glob
import json
import math
import os
import sys
from collections import defaultdict

import numpy as np

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
from csi_ml_config import TARGET_COLUMN, WINDOW_SIZE, is_usable_effective_cqi

def _sigmoid(x):
    return 1.0 / (1.0 + np.exp(-x))

def load_model(path):
    with open(path) as fh:
        return json.load(fh)

def wiener_predict(model, window):
    p, mu, sigma = model["p"], model["mu"], model["sigma"]
    coef = np.array(model["coef"], dtype=np.float64).reshape(model["t_out"], p)
    intercept = np.array(model["intercept"], dtype=np.float64)
    xs = (np.array(window, dtype=np.float64) - mu) / sigma
    y_std = intercept[0] + float(coef[0] @ xs)
    return y_std * sigma + mu

def gru_predict(model, window):
    p, d, mu, sigma = model["p"], model["d"], model["mu"], model["sigma"]
    W_z = np.array(model["w_z"]).reshape(d, 1); U_z = np.array(model["u_z"]).reshape(d, d); b_z = np.array(model["b_z"])
    W_r = np.array(model["w_r"]).reshape(d, 1); U_r = np.array(model["u_r"]).reshape(d, d); b_r = np.array(model["b_r"])
    W_h = np.array(model["w_h"]).reshape(d, 1); U_h = np.array(model["u_h"]).reshape(d, d); b_h = np.array(model["b_h"])
    W_out = np.array(model["w_out"]).reshape(model["t_out"], d); b_out = np.array(model["b_out"])
    h = np.zeros(d)
    xs = (np.array(window, dtype=np.float64) - mu) / sigma
    for t in range(p):
        x = xs[t]
        z = _sigmoid(W_z[:, 0] * x + U_z @ h + b_z)
        r = _sigmoid(W_r[:, 0] * x + U_r @ h + b_r)
        hh = np.tanh(W_h[:, 0] * x + U_h @ (r * h) + b_h)
        h = (1 - z) * h + z * hh
    y_std = float(W_out[0] @ h + b_out[0])
    return y_std * sigma + mu

def load_series_by_scenario(path):
    out = defaultdict(lambda: defaultdict(list))
    with open(path, newline="") as fh:
        reader = csv.DictReader(fh)
        for row in reader:
            if not is_usable_effective_cqi(row.get(TARGET_COLUMN)):
                continue
            scen = row.get("scenario", "default") or "default"
            out[scen][(row["rnti"], row["ue_index"])].append(float(row[TARGET_COLUMN]))
    return out

def _nearest_rank(se, num, den):
    n = len(se)
    if n == 0:
        return float("nan")
    return float(se[max(1, min(n, round(num * (n + 1) / den))) - 1])

def score(errors, sigma):
    errors = np.asarray(errors)
    if len(errors) == 0:
        return None
    se = np.sort(np.abs(errors))
    mse = float(np.mean(errors ** 2))
    mse_std = mse / (sigma ** 2) if sigma else mse
    return {
        "n": len(errors),
        "mae": float(np.mean(np.abs(errors))),
        "mse": mse,
        "mse_db": 10.0 * math.log10(mse_std) if mse_std > 0 else float("-inf"),
        "median": _nearest_rank(se, 1, 2),
        "p75": _nearest_rank(se, 3, 4),
        "p95": _nearest_rank(se, 19, 20),
    }

def replay_scenario(per_ue, window_size, wiener, gru, sigma_for_db):
    err = {"zoh": [], "wiener": [], "gru": []}
    for _key, series in per_ue.items():
        for i in range(len(series) - window_size):
            window = series[i : i + window_size]
            truth = series[i + window_size]
            err["zoh"].append(window[-1] - truth)
            if wiener is not None:
                err["wiener"].append(wiener_predict(wiener, window) - truth)
            if gru is not None:
                err["gru"].append(gru_predict(gru, window) - truth)
    return {k: score(v, sigma_for_db) for k, v in err.items() if v}

def _row(name, s):
    if s is None:
        return f"  {name:8s} : (no data)"
    return (f"  {name:8s} : MAE={s['mae']:.3f}  MSE={s['mse']:.3f}  MSE={s['mse_db']:6.2f}dB  "
            f"median={s['median']:.3f}  p75={s['p75']:.3f}  p95={s['p95']:.3f}  (n={s['n']})")

def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("csv", nargs="*", help="dataset CSV(s); default: ml/datasets/csi_ml_dataset_*.csv")
    ap.add_argument("--wiener", default=None, help="Wiener .model file")
    ap.add_argument("--gru", default=None, help="GRU .model file")
    ap.add_argument("--window-size", type=int, default=WINDOW_SIZE)
    args = ap.parse_args()

    paths = args.csv or sorted(glob.glob("ml/datasets/csi_ml_dataset_*.csv"))
    if not paths:
        sys.exit("no dataset CSVs found")
    if not args.wiener and not args.gru:
        sys.exit("pass at least one of --wiener / --gru")

    wiener = load_model(args.wiener) if args.wiener else None
    gru = load_model(args.gru) if args.gru else None
    sigma_for_db = (wiener or gru)["sigma"]

    merged = defaultdict(lambda: defaultdict(list))
    for p in paths:
        for scen, per_ue in load_series_by_scenario(p).items():
            for key, series in per_ue.items():
                merged[scen][key].extend(series)

    print(f"CSI predictor offline replay — files={len(paths)}, scenarios={list(merged.keys())}")
    print(f"models: wiener={'yes' if wiener else 'no'} gru={'yes' if gru else 'no'} P={args.window_size}\n")

    summary = []
    for scen in sorted(merged.keys()):
        res = replay_scenario(merged[scen], args.window_size, wiener, gru, sigma_for_db)
        print(f"[scenario: {scen}]")
        for name in ("zoh", "wiener", "gru"):
            print(_row(name, res.get(name)))

        zoh = res.get("zoh")
        for m in ("wiener", "gru"):
            if res.get(m) and zoh:
                gain = zoh["mae"] - res[m]["mae"]
                verdict = "BETTER" if gain > 0 else "no gain"
                print(f"    -> {m} vs ZOH: MAE {gain:+.3f} CQI ({verdict})")
                summary.append((scen, m, res[m]["mae"], zoh["mae"], gain))
        print()

    if summary:
        print("Cross-scenario summary (MAE, CQI units):")
        print(f"  {'scenario':16s} {'model':8s} {'ml_mae':>8s} {'zoh_mae':>8s} {'gain':>8s}")
        for scen, m, mlmae, zohmae, gain in summary:
            print(f"  {scen:16s} {m:8s} {mlmae:8.3f} {zohmae:8.3f} {gain:+8.3f}")

if __name__ == "__main__":
    main()
