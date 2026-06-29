#!/usr/bin/env python3
import argparse
import csv
import glob
import math
from collections import defaultdict


def fnum(x):
    try:
        return float(x)
    except (TypeError, ValueError):
        return math.nan


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--data-dir", default="ml/datasets")
    ap.add_argument("--min-sinr-rows", type=int, default=300)
    args = ap.parse_args()

    files = sorted(glob.glob(f"{args.data_dir}/ul_mcs_dataset_*.csv"))
    groups = {0: dict(n=0, ok=0, dlv=0.0, mcs=0.0), 1: dict(n=0, ok=0, dlv=0.0, mcs=0.0)}
    sinr = defaultdict(lambda: {0: [0, 0, 0.0], 1: [0, 0, 0.0]})
    per_mcs = defaultdict(lambda: [0, 0])
    has_split = False

    for p in files:
        with open(p, newline="") as fh:
            for r in csv.DictReader(fh):
                try:
                    if int(float(r["nof_retxs"])) != 0:
                        continue
                    crc = int(float(r["crc_success"]))
                    mcs = int(float(r["mcs"]))
                    tbs = fnum(r["tbs_bytes"])
                except (KeyError, ValueError, TypeError):
                    continue
                per_mcs[mcs][0] += 1
                per_mcs[mcs][1] += 1 - crc
                mlc = r.get("ml_in_control")
                if mlc in (None, ""):
                    continue
                has_split = True
                c = 1 if int(float(mlc)) == 1 else 0
                g = groups[c]
                g["n"] += 1
                g["ok"] += crc
                g["mcs"] += mcs
                if crc:
                    g["dlv"] += tbs
                s = fnum(r.get("pusch_avg_sinr_db", "nan"))
                if not math.isnan(s):
                    b = int(round(s / 3.0) * 3)
                    sb = sinr[b][c]
                    sb[0] += 1
                    sb[1] += crc
                    if crc:
                        sb[2] += tbs

    print(f"files={len(files)}")
    if has_split:
        def line(tag, g):
            if not g["n"]:
                print(f"  {tag}: none")
                return
            print(f"  {tag:5s} n={g['n']:>8,}  BLER={100*(1-g['ok']/g['n']):5.1f}%  "
                  f"meanMCS={g['mcs']/g['n']:5.1f}  delivered/tx={g['dlv']/g['n']:7.0f}B")
        print("\nOLLA vs ML (newTx):")
        line("OLLA", groups[0])
        line("ML", groups[1])
        print("\nSINR-matched delivered B/tx:")
        print(f"  {'SINR':>4} | {'OLLA n':>7} {'BLER%':>6} {'dlv':>6} | {'ML n':>7} {'BLER%':>6} {'dlv':>6} | {'ML vs OLLA':>10}")
        for b in sorted(sinr):
            o, m = sinr[b][0], sinr[b][1]
            if o[0] < args.min_sinr_rows or m[0] < args.min_sinr_rows:
                continue
            od, md = o[2] / o[0], m[2] / m[0]
            print(f"  {b:>4} | {o[0]:>7,} {100*(1-o[1]/o[0]):>6.1f} {od:>6.0f} | "
                  f"{m[0]:>7,} {100*(1-m[1]/m[0]):>6.1f} {md:>6.0f} | {100*(md-od)/max(od,1e-9):>+9.1f}%")

    print("\nper-MCS coverage (newTx):")
    print(f"  {'MCS':>4} {'newTx':>9} {'BLER%':>7}")
    for m in sorted(per_mcs):
        n, fl = per_mcs[m]
        print(f"  {m:>4} {n:>9} {100*fl/max(n,1):>7.1f}")


if __name__ == "__main__":
    main()
