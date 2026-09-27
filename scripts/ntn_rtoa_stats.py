#!/usr/bin/env python3
"""UL-RTOA statistics from the OAI LMF's log, for the NTN positioning runs.

The LMF prints one line per measurement it receives:

    [2026-09-17 20:21:18.950] [lmf_app] [info] measurement: gnbId: 0x19b trpId: 1 k1: 492512 sfn: 912 slot: 0

k1 is the UL-RTOA of TS 38.455 at resolution k1, i.e. steps of 2*Tc = 1.0173 ns, biased so that a ToA of zero
is k1 = 492512 (see phy_time_unit::to_ul_rtoa() in the gNB: k1 = ((Tc + 985023) >> 1) + 1). This script undoes
that bias and reports the residual in ns and in metres of one-way range.

    scripts/ntn_rtoa_stats.py                      # read `docker logs oai-lmf`
    scripts/ntn_rtoa_stats.py /path/to/lmf.log     # or a saved log
    scripts/ntn_rtoa_stats.py --bucket 120         # trend, in windows of N seconds

A run is anything separated from the next by more than 60 s of silence, so several runs in one log stay apart.

CAVEAT, and it matters: when the UE transmits no SRS at all - gone, released, or past the satellite's horizon -
the gNB still reports k1 = 492512, which is exactly 0 ns and indistinguishable from perfect alignment. Those
samples are counted as "silent" here and left out of the statistics, which is the best that can be done from
outside the gNB. Cross-check against "epre=" on the gNB's own "SRS:" lines before trusting a quiet run.
"""
import argparse, re, statistics, subprocess, sys
from datetime import datetime

K1_ZERO = 492512  # k1 reported for a ToA of exactly zero
NS_PER_K1 = 1e9 / (4096 * 480000 / 2)  # 2*Tc = 1.0173 ns
M_PER_NS = 0.299792458

LINE = re.compile(
    r"\[(\d{4}-\d\d-\d\d \d\d:\d\d:\d\d)\.(\d+)\].*measurement: gnbId: (0x[0-9a-f]+) trpId: (\d+) k1: (\d+)")


def read(source):
    if source:
        return open(source, errors="replace")
    out = subprocess.run(["docker", "logs", "oai-lmf"], capture_output=True, text=True, errors="replace")
    return (out.stdout + out.stderr).splitlines()


def parse(lines):
    for line in lines:
        m = LINE.search(line)
        if m:
            ts = datetime.strptime(m.group(1), "%Y-%m-%d %H:%M:%S")
            yield ts, m.group(3), int(m.group(4)), int(m.group(5))


def describe(samples, label):
    live = [ns for ns in samples if ns is not None]
    print(f"{label}  measurements={len(samples)}  with SRS={len(live)}  silent={len(samples) - len(live)}")
    if not live:
        print("    no live measurement - the UE was not transmitting SRS in this window")
        return
    sd = statistics.pstdev(live) if len(live) > 1 else 0.0
    print(f"    ToA ns: mean {statistics.mean(live):+8.0f}  sd {sd:6.0f}  min {min(live):+8.0f}  max {max(live):+8.0f}")
    print(f"    one-way range equivalent: mean {statistics.mean(live) * M_PER_NS:+.1f} m, "
          f"spread +-{max(abs(min(live)), abs(max(live))) * M_PER_NS:.0f} m")


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("log", nargs="?", help="LMF log file; default is `docker logs oai-lmf`")
    ap.add_argument("--bucket", type=int, metavar="SECONDS",
                    help="also print a trend in windows of this many seconds")
    ap.add_argument("--gnb", help="only this gnbId, e.g. 0x19b")
    args = ap.parse_args()

    rows = [r for r in parse(read(args.log)) if args.gnb is None or r[1] == args.gnb]
    if not rows:
        sys.exit("no measurement lines found")

    # Split into runs on a gap of more than a minute.
    runs = [[rows[0]]]
    for row in rows[1:]:
        if (row[0] - runs[-1][-1][0]).total_seconds() > 60:
            runs.append([])
        runs[-1].append(row)

    for run in runs:
        # None marks a silent occasion, so it can be counted without polluting the mean.
        samples = [None if k1 == K1_ZERO else (k1 - K1_ZERO) * NS_PER_K1 for _, _, _, k1 in run]
        describe(samples, f"run {run[0][0]:%H:%M:%S}..{run[-1][0]:%H:%M:%S}")
        if args.bucket:
            t0 = run[0][0]
            buckets = {}
            for (ts, _, _, _), ns in zip(run, samples):
                buckets.setdefault(int((ts - t0).total_seconds() // args.bucket), []).append(ns)
            for b in sorted(buckets):
                describe(buckets[b], f"    +{b * args.bucket:4d}s")
        print()


if __name__ == "__main__":
    main()
