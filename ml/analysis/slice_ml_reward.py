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
import os
import statistics as st
import sys
from collections import defaultdict

_HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(_HERE, "..", "training"))

try:
    from train_slicemanager_actor import (
        LAMBDA_EMBB,
        LAMBDA_SP,
        LAMBDA_URLLC,
        UPSILON_EMBB,
        UPSILON_URLLC,
        load_slicemanager_rows,
        utility,
    )
except ImportError as exc:
    sys.exit(
        f"cannot import the reward definition from ml/training/train_slicemanager_actor.py: {exc}\n"
        "That file is the single source of truth for eq. (7); this script will not guess at it."
    )

DEFAULT_DIR = os.path.join(_HERE, "..", "datasets", "slice_datasets")


def fnum(row, col):
    v = row.get(col, "")
    if v is None or v == "":
        return None
    try:
        return float(v)
    except ValueError:
        return None


def explain():
    print("  reward = {:.2f} * embb_Mbps".format(LAMBDA_SP))
    print("           - {:.1f} * max(0, {:.2f} - embb_ssr)     <- penalty, only if eMBB SSR is below target"
          .format(LAMBDA_EMBB, UPSILON_EMBB))
    print("           - {:.1f} * max(0, {:.2f} - urllc_ssr)    <- penalty, only if URLLC SSR is below target"
          .format(LAMBDA_URLLC, UPSILON_URLLC))
    print()
    print("  SSR = slice satisfaction ratio: the fraction of that slice's demand the scheduler met")
    print("        (eq. 14 for eMBB, eq. 21 for URLLC). Both are computed in the DU and logged per")
    print("        period as ssr_embb / ssr_urllc.")
    print()
    print("  Reading the numbers:")
    print("    * reward is per 1-second period; higher is better, and it is usually NEGATIVE because")
    print("      an unmet SLA costs far more (weight 5.0) than throughput earns (weight 0.01).")
    print("    * reward == 0 would mean both SSR targets met with zero eMBB throughput.")
    print("    * the three columns after 'reward' sum to it, so you can see which term dominates.")
    print("    * a run that scores well purely on 'thr' while both penalties are large is NOT good --")
    print("      it bought eMBB bytes by starving the slices.")
    print()


def collect(paths, embb_sst, urllc_sst, key_by):
    records = load_slicemanager_rows(paths, embb_sst=embb_sst, urllc_sst=urllc_sst)
    buckets = defaultdict(list)
    for rec in records:
        key = rec["scenario"] if key_by == "scenario" else os.path.basename(rec["run"])[23:-4]
        buckets[key].append(rec)
    return buckets


def radio_side_table(paths, embb_sst, urllc_sst, key_by="scenario"):
    out = defaultdict(lambda: defaultdict(list))
    for slicemanager_path in paths:
        ue_path = slicemanager_path.replace("slice_ml_slicemanager_", "slice_ml_ue_")
        if not os.path.exists(ue_path):
            continue
        file_key = os.path.basename(slicemanager_path)[23:-4]
        with open(ue_path, newline="") as fh:
            for row in csv.DictReader(fh):
                try:
                    sst = int(row.get("sst") or "")
                except (TypeError, ValueError):
                    continue
                if sst not in (embb_sst, urllc_sst):
                    continue
                key = row.get("scenario", "default") if key_by == "scenario" else file_key
                out[key][sst].append(row)
    return out


def mean(vals):
    vals = [v for v in vals if v is not None]
    return st.mean(vals) if vals else float("nan")


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--dir", default=DEFAULT_DIR, help="directory holding slice_ml_slicemanager_*.csv")
    ap.add_argument("--scenario", default="", help="only scenarios containing this substring")
    ap.add_argument("--by", choices=["scenario", "file"], default="scenario", help="row granularity")
    ap.add_argument("--embb-sst", type=int, default=1)
    ap.add_argument("--urllc-sst", type=int, default=2)
    ap.add_argument("--quiet", action="store_true", help="skip the explanation header")
    args = ap.parse_args()

    paths = sorted(glob.glob(os.path.join(args.dir, "slice_ml_slicemanager_*.csv")))
    if not paths:
        sys.exit(f"no slice_ml_slicemanager_*.csv found under {args.dir}")

    if not args.quiet:
        explain()

    buckets = collect(paths, args.embb_sst, args.urllc_sst, args.by)
    if args.scenario:
        buckets = {k: v for k, v in buckets.items() if args.scenario in k}
    if not buckets:
        sys.exit(f"no periods matched --scenario {args.scenario!r}")

    rows = []
    for key, recs in buckets.items():
        embb_mbps = [r["obs"][0] for r in recs]
        embb_ssr = [r["obs"][2] for r in recs]
        urllc_ssr = [r["obs"][3] for r in recs]
        rewards = [r["utility"] for r in recs]

        thr = mean([LAMBDA_SP * m for m in embb_mbps])
        pen_e = mean([-LAMBDA_EMBB * max(0.0, UPSILON_EMBB - s) for s in embb_ssr])
        pen_u = mean([-LAMBDA_URLLC * max(0.0, UPSILON_URLLC - s) for s in urllc_ssr])

        rows.append(
            {
                "key": key,
                "n": len(recs),
                "reward": mean(rewards),
                "p10": sorted(rewards)[int(0.10 * (len(rewards) - 1))],
                "thr": thr,
                "pen_e": pen_e,
                "pen_u": pen_u,
                "embb_mbps": mean(embb_mbps),
                "ssr_e": mean(embb_ssr),
                "ssr_u": mean(urllc_ssr),
                "hit_e": 100.0 * sum(s >= UPSILON_EMBB for s in embb_ssr) / len(embb_ssr),
                "hit_u": 100.0 * sum(s >= UPSILON_URLLC for s in urllc_ssr) / len(urllc_ssr),
            }
        )

    rows.sort(key=lambda r: r["reward"], reverse=True)

    hdr = (f"{'scenario':<34} {'n':>5} {'reward':>8} {'p10':>8} | {'thr':>7} {'pen_eMBB':>9} {'pen_URLLC':>10} | "
           f"{'ssr_e':>6} {'ssr_u':>6} {'hit_e%':>7} {'hit_u%':>7} {'eMBB_Mb':>8}")
    print(hdr)
    print("-" * len(hdr))
    for r in rows:
        print(f"{r['key'][:34]:<34} {r['n']:>5} {r['reward']:>8.3f} {r['p10']:>8.3f} | "
              f"{r['thr']:>7.3f} {r['pen_e']:>9.3f} {r['pen_u']:>10.3f} | "
              f"{r['ssr_e']:>6.3f} {r['ssr_u']:>6.3f} {r['hit_e']:>7.1f} {r['hit_u']:>7.1f} {r['embb_mbps']:>8.2f}")

    print()
    best = rows[0]
    print(f"BEST: {best['key']}  reward {best['reward']:.3f} over {best['n']} periods")
    driver = min(("eMBB SLA", best["pen_e"]), ("URLLC SLA", best["pen_u"]), key=lambda t: t[1])
    if driver[1] < -0.001:
        print(f"      still losing {abs(driver[1]):.3f}/period to the {driver[0]} penalty -- "
              f"that term is what any further work has to attack.")
    else:
        print("      both SSR targets met; reward is throughput-driven.")

    if True:
        radio = radio_side_table(paths, args.embb_sst, args.urllc_sst, args.by)
        radio = {k: v for k, v in radio.items() if k in buckets}
        if radio:
            print()
            print("RADIO CONTEXT (not part of eq. 7 -- explains WHY a penalty term is what it is)")
            h2 = (f"{'scenario':<34} {'sst':>4} {'dl_Mbps':>8} {'ul_Mbps':>8} {'dl_bler':>8} "
                  f"{'ul_bler':>8} {'dl_mcs':>7} {'ul_mcs':>7}")
            print(h2)
            print("-" * len(h2))
            for key in [r["key"] for r in rows]:
                for sst in sorted(radio.get(key, {})):
                    g = radio[key][sst]
                    print(f"{key[:34]:<34} {sst:>4} "
                          f"{mean([fnum(r, 'dl_brate_kbps') for r in g]) / 1000:>8.2f} "
                          f"{mean([fnum(r, 'ul_brate_kbps') for r in g]) / 1000:>8.2f} "
                          f"{mean([fnum(r, 'dl_bler_pct') for r in g]):>8.2f} "
                          f"{mean([fnum(r, 'ul_bler_pct') for r in g]):>8.2f} "
                          f"{mean([fnum(r, 'dl_mcs') for r in g]):>7.2f} "
                          f"{mean([fnum(r, 'ul_mcs') for r in g]):>7.2f}")


if __name__ == "__main__":
    main()
