#!/usr/bin/env python3
"""Plot the latency-vs-#BSRs trade-off from real runs: fixed-timer baselines plus the ML run.

Each argument is LABEL=run.csv, where LABEL is a fixed timer ("sf10", "sf20", ...) or "ML". All runs must
carry the same traffic for the comparison to be valid.

Usage:
    python3 ml/analysis/plot_tradeoff.py sf10=a.csv sf40=b.csv ML=c.csv [--out tradeoff.png]
"""

import argparse
import os
import re
import sys

import matplotlib
matplotlib.use("Agg")
import matplotlib.pyplot as plt

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from analyze_bsr_run import run_metrics


def is_ml(label):
    return label.strip().lower() in ("ml", "svr-rff", "svrrff", "adaptive")


def sf_value(label):
    m = re.match(r"sf(\d+)", label.strip().lower())
    return int(m.group(1)) if m else None


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("runs", nargs="+", help="LABEL=run.csv pairs (LABEL = sfN for fixed timers, or ML)")
    ap.add_argument("--metric", choices=["periodic", "total"], default="periodic",
                    help="which BSR count on the y-axis (default: periodic — the overhead the timer controls)")
    ap.add_argument("--baseline", default=None, help="label to use as the reference for headline deltas (default: smallest sfN)")
    ap.add_argument("--out", default="ml/analysis/tradeoff.png", help="output PNG path")
    args = ap.parse_args()

    pts = []
    for item in args.runs:
        if "=" not in item:
            sys.exit(f"expected LABEL=run.csv, got: {item}")
        label, path = item.split("=", 1)
        m = run_metrics(path)
        if m is None:
            print(f"skip {label}: no usable rows in {path}", file=sys.stderr)
            continue
        y = m["periodic_bsr_per_s"] if args.metric == "periodic" else m["total_bsr_per_s"]
        pts.append({"label": label, "ml": is_ml(label), "sf": sf_value(label),
                    "lat": m["latency_ms"], "y": y, "m": m})

    if not pts:
        sys.exit("no valid runs")

    fixed = sorted([p for p in pts if not p["ml"]], key=lambda p: (p["sf"] is None, p["sf"] or 0))
    ml_pts = [p for p in pts if p["ml"]]

    # baseline for headline deltas
    base = None
    if args.baseline:
        base = next((p for p in pts if p["label"] == args.baseline), None)
    if base is None and fixed:
        base = fixed[0]

    # ---- headline table ----
    ycol = "periodic BSRs/s" if args.metric == "periodic" else "total BSRs/s"
    print(f"\n{'run':<10}{'latency_ms':>12}{ycol:>18}{'vs '+base['label'] if base else '':>26}")
    print("-" * 66)
    for p in fixed + ml_pts:
        delta = ""
        if base and p is not base:
            dlat = p["lat"] - base["lat"]
            dbsr = 100.0 * (p["y"] - base["y"]) / base["y"] if base["y"] else float("nan")
            delta = f"{dlat:+.1f} ms, {dbsr:+.1f}% BSRs"
        star = " *ML*" if p["ml"] else ""
        print(f"{p['label']:<10}{p['lat']:>12.1f}{p['y']:>18.2f}{delta:>26}{star}")

    # ---- plot ----
    fig, ax = plt.subplots(figsize=(7.5, 5.2))
    if fixed:
        fx = [p["lat"] for p in fixed]
        fy = [p["y"] for p in fixed]
        ax.plot(fx, fy, "o-", color="#1f77b4", label="fixed periodicBSR-Timer", zorder=2)
        for p in fixed:
            ax.annotate(p["label"], (p["lat"], p["y"]), textcoords="offset points", xytext=(6, 5), fontsize=9)
    for p in ml_pts:
        ax.scatter([p["lat"]], [p["y"]], s=140, color="#d62728", marker="*", zorder=3, label=p["label"])
        ax.annotate(p["label"], (p["lat"], p["y"]), textcoords="offset points", xytext=(6, -12),
                    fontsize=10, color="#d62728", fontweight="bold")
    ax.set_xlabel("UL latency estimate  [ms]  (gNB-side: D1 Little's-law + D2.1)")
    ax.set_ylabel(f"{ycol}  (control resource usage)")
    ax.set_title("BSR periodicity: latency vs. signalling overhead\n(lower-left = better)")
    ax.grid(True, alpha=0.3)
    ax.legend()
    fig.tight_layout()
    fig.savefig(args.out, dpi=130)
    print(f"\nsaved plot -> {args.out}")


if __name__ == "__main__":
    main()
