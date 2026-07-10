#!/usr/bin/env python3
"""Side-by-side comparison of BSR runs. Each argument is LABEL=run.csv, where LABEL is a fixed timer
("sf10", "sf64", ...) or "ML".

Reports two latency views: the reporting-delay bound (clean, valid even if traffic differs) and the
Little's-law D1 (volume-confounded — read the traffic match-check row first).

Usage:
    python3 ml/analysis/compare_runs.py sf10=a.csv sf64=b.csv ML=c.csv
"""

import re
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from analyze_bsr_run import run_metrics


def sf_value(label):
    m = re.match(r"sf(\d+)$", label.strip().lower())
    return int(m.group(1)) if m else None


def is_ml(label):
    return label.strip().lower() in ("ml", "svr-rff", "svrrff", "adaptive")


def main():
    if len(sys.argv) < 3:
        sys.exit(__doc__)
    cols = []
    for item in sys.argv[1:]:
        if "=" not in item:
            sys.exit(f"expected LABEL=run.csv, got: {item}")
        label, path = item.split("=", 1)
        m = run_metrics(path)
        if m is None:
            print(f"skip {label}: no usable rows in {path}", file=sys.stderr)
            continue
        sf = sf_value(label)
        # reporting-delay bound: fixed value for a static run, mean applied timer for the ML run
        bound = float(sf) if sf is not None else m["ml_bound_ms"]
        cols.append((label, m, bound))

    def row(name, fmt, key=None, fn=None):
        vals = []
        for _, m, bound in cols:
            v = fn(m, bound) if fn else m.get(key)
            vals.append("n/a" if v is None or (isinstance(v, float) and v != v) else fmt.format(v))
        print(f"  {name:<36}" + "".join(f"{v:>14}" for v in vals))

    print("\n" + " " * 38 + "".join(f"{lab:>14}" for lab, _, _ in cols))
    print("  " + "-" * (36 + 14 * len(cols)))
    print("  TRAFFIC MATCH CHECK (must be similar for D1 / overhead to be comparable)")
    row("duration (s)", "{:.0f}", "duration_s")
    row("total UL data (MB)", "{:.0f}", "total_mb")
    row("mean UL rate (kbps)", "{:.0f}", "mean_kbps")
    row("median interarrival (ms)", "{:.0f}", "median_ia_ms")
    row("buffer-occupied (%)", "{:.0f}", "occupied_pct")
    print("  " + "-" * (36 + 14 * len(cols)))
    print("  OVERHEAD (control resource usage)")
    row("periodic BSRs/s", "{:.2f}", "periodic_bsr_per_s")
    row("effective avg timer (ms)", "{:.0f}", "eff_timer_ms")
    print("  " + "-" * (36 + 14 * len(cols)))
    print("  LATENCY — clean (valid even if traffic differs)")
    row("reporting-delay bound (ms)", "{:.0f}", fn=lambda m, b: b)
    row("  avg reporting delay ~bound/2 (ms)", "{:.0f}", fn=lambda m, b: (b / 2 if b else None))
    print("  LATENCY — volume-confounded (only if match-check OK)")
    row("D1 Little's-law (ms)", "{:.0f}", "d1_ms")
    print("  " + "-" * (36 + 14 * len(cols)))

    # headline vs the ML column, on the clean metric
    ml = next(((lab, m, b) for lab, m, b in cols if is_ml(lab)), None)
    if ml and ml[2]:
        print(f"\n  ML reporting-delay bound = {ml[2]:.0f} ms. Vs each conservative static timer:")
        for lab, m, b in cols:
            if is_ml(lab) or b is None:
                continue
            if b > ml[2]:
                print(f"    vs {lab}: ML is {100*(1-ml[2]/b):.0f}% LOWER reporting delay  ({b:.0f} -> {ml[2]:.0f} ms)")
            else:
                print(f"    vs {lab}: ML is {100*(ml[2]/b-1):.0f}% higher (ML leans to overhead here; {lab} is the responsive one)")


if __name__ == "__main__":
    main()
