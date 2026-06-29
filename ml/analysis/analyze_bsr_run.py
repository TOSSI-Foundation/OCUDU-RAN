#!/usr/bin/env python3
"""Per-run BSR metrics (counts by trigger type, overhead, latency estimate).

Usage:
    python3 ml/analysis/analyze_bsr_run.py run.csv [run2.csv ...]
"""

import csv
import math
import sys
from collections import Counter, defaultdict

TYPE_NAMES = {"1": "regular", "2": "periodic", "3": "padding", "0": "none"}


def absolute_times_ms(rows):
    """Per-row absolute time in ms from SFN (10 ms) + subframe (1 ms), SFN-wrap corrected."""
    out, base, prev = [], 0, None
    for r in rows:
        sfn, sf = int(r["sfn"]), int(r["subframe"])
        if prev is not None and sfn < prev - 512:  # wrap 1023 -> 0
            base += 1024
        prev = sfn
        out.append((base + sfn) * 10 + sf)
    return out


def _pctl(v, p):
    if not v:
        return float("nan")
    v = sorted(v)
    k = (len(v) - 1) * p / 100
    lo, hi = math.floor(k), math.ceil(k)
    return v[lo] if lo == hi else v[lo] + (v[hi] - v[lo]) * (k - lo)


def _load_rows(path):
    rows = []
    with open(path, newline="") as fh:
        for r in csv.DictReader(fh):
            if r.get("bsr_trigger_type") in (None, ""):
                continue
            rows.append(r)
    return rows


def latency_estimate(rows, times_ms):
    """Per-UE gNB-side UL latency estimate: D1 (Little's law, E[Q]/lambda) + D2.1 (over-air).

    D1 is volume-confounded — only compare it across runs carrying the same traffic.
    """
    spm = 2 ** int(rows[0]["numerology"])  # slots per ms
    ue_rows = defaultdict(list)
    for i, r in enumerate(rows):
        ue_rows[(r["rnti"], r["ue_index"])].append(i)

    per_ue = {}
    for ue, idxs in ue_rows.items():
        if len(idxs) < 2:
            continue
        q_num = dt_sum = 0.0
        for k in range(len(idxs) - 1):
            dt = times_ms[idxs[k + 1]] - times_ms[idxs[k]]
            if dt <= 0:
                continue
            q_num += float(rows[idxs[k]].get("pending_ul_bytes") or 0.0) * dt
            dt_sum += dt
        brate = [float(rows[i]["ul_brate_kbps"]) for i in idxs if rows[i].get("ul_brate_kbps") not in (None, "")]
        d21 = [float(rows[i]["last_ul_over_air_delay_slots"]) / spm for i in idxs
               if rows[i].get("has_last_ul_over_air_delay") == "1"
               and rows[i].get("last_ul_over_air_delay_slots") not in (None, "")]
        active = [b for b in brate if b > 0]
        if dt_sum <= 0 or not active:
            continue
        qbar = q_num / dt_sum
        mean_bps = (sum(active) / len(active)) * 1000.0 / 8.0
        w_ms = 1000.0 * qbar / mean_bps if mean_bps > 0 else 0.0
        d21_mean = sum(d21) / len(d21) if d21 else 0.0
        per_ue[ue] = {
            "qbar_bytes": qbar, "mean_kbps": mean_bps * 8.0 / 1000.0, "W_ms": w_ms,
            "d21_mean_ms": d21_mean, "d21_p95_ms": _pctl(d21, 95) if d21 else 0.0,
            "total_ms": w_ms + d21_mean, "nrows": len(idxs),
        }
    return per_ue


def run_metrics(path):
    """Run-level metrics. Aggregate latency is throughput-weighted so idle UEs do not dominate."""
    rows = _load_rows(path)
    if len(rows) < 2:
        return None
    times = absolute_times_ms(rows)
    dur = max(1e-9, (times[-1] - times[0]) / 1000.0)
    by_type = Counter(r["bsr_trigger_type"] for r in rows)
    total = len(rows)
    periodic = by_type.get("2", 0)
    per_ue = latency_estimate(rows, times)

    wsum = sum(e["mean_kbps"] for e in per_ue.values())
    if wsum > 0:
        d1 = sum(e["W_ms"] * e["mean_kbps"] for e in per_ue.values()) / wsum
        d2 = sum(e["d21_mean_ms"] * e["mean_kbps"] for e in per_ue.values()) / wsum
    else:
        d1, d2 = 0.0, 0.0

    # traffic-context fields (for the same-traffic match check) + the clean, volume-independent latency metric
    spm = 2 ** int(rows[0]["numerology"])
    ia = sorted(float(r["interarrival_slots"]) / spm for r in rows
                if r["bsr_trigger_type"] == "1" and r.get("has_interarrival") == "1")
    active = [float(r["ul_brate_kbps"]) for r in rows
              if r.get("ul_brate_kbps") not in (None, "") and float(r["ul_brate_kbps"]) > 0]
    mean_kbps = sum(active) / len(active) if active else 0.0
    occ_ms = sum(times[k + 1] - times[k] for k in range(len(rows) - 1)
                 if float(rows[k].get("pending_ul_bytes") or 0) > 0)
    # reporting-delay bound = applied periodicBSR-Timer (TS 38.321 clause 5.4.5). ML runs: mean applied timer.
    # Static runs: the fixed configured value, supplied via compare_runs.py (not logged per row).
    pred = [int(r["predicted_periodicity_subframes"]) for r in rows if r.get("has_predicted_periodicity") == "1"]
    return {
        "path": path, "duration_s": dur, "n_ues": len(set((r["rnti"], r["ue_index"]) for r in rows)),
        "by_type": dict(by_type), "total_bsr": total, "periodic_bsr": periodic,
        "total_bsr_per_s": total / dur, "periodic_bsr_per_s": periodic / dur,
        "d1_ms": d1, "d2_ms": d2, "latency_ms": d1 + d2, "per_ue": per_ue,
        "total_mb": mean_kbps * 1000 / 8 * dur / 1e6, "mean_kbps": mean_kbps,
        "occupied_pct": 100 * occ_ms / (dur * 1000), "median_ia_ms": (ia[len(ia) // 2] if ia else float("nan")),
        "eff_timer_ms": (occ_ms / periodic if periodic else float("nan")),
        "ml_bound_ms": (sum(pred) / len(pred) if pred else None), "n_predictions": len(pred),
    }


def analyze(path):
    m = run_metrics(path)
    if m is None:
        print(f"{path}: no usable rows")
        return
    print(f"\n=== {path} ===")
    print(f"  duration: {m['duration_s']:.0f} s   UEs: {m['n_ues']}   total BSRs: {m['total_bsr']}")
    print(f"  {'type':<10}{'count':>8}{'share':>8}{'per-sec':>10}")
    for t in ("1", "2", "3", "0"):
        c = m["by_type"].get(t, 0)
        if c:
            print(f"  {TYPE_NAMES[t]:<10}{c:>8}{100*c/m['total_bsr']:>7.1f}%{c/m['duration_s']:>10.2f}")
    print(f"  ----> RESOURCE-USAGE (#BSRs): total={m['total_bsr_per_s']:.2f}/s  "
          f"periodic={m['periodic_bsr_per_s']:.2f}/s")
    print(f"  ----> LATENCY ESTIMATE (gNB-side, same-traffic A/B only): "
          f"D1={m['d1_ms']:.1f}ms + D2.1={m['d2_ms']:.2f}ms = {m['latency_ms']:.1f}ms")
    print(f"  per-UE: backlog(KB) rate(kbps) D1_ms D2.1_ms")
    for (rnti, uei), e in sorted(m["per_ue"].items(), key=lambda kv: -kv[1]["mean_kbps"]):
        print(f"     rnti={rnti:<7} ue={uei}: {e['qbar_bytes']/1000:>8.1f} {e['mean_kbps']:>9.1f} "
              f"{e['W_ms']:>7.1f} {e['d21_mean_ms']:>6.2f}")


def main():
    if len(sys.argv) < 2:
        sys.exit(__doc__)
    for p in sys.argv[1:]:
        analyze(p)


if __name__ == "__main__":
    main()
