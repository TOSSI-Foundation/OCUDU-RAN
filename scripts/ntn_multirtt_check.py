#!/usr/bin/env python3
"""Check the LMF's NR Multi-RTT round trips against the round trip the gNB was actually emulating.

The LMF logs one line per paired round (TS 38.305 8.10, RTT = UE Rx-Tx + gNB Rx-Tx):

    Multi-RTT: UE Rx-Tx 18430.1 us (...) + gNB Rx-Tx -13.1 us at sfn 240 (PRS slot 5, SRS slot 0) = RTT ... us -> ... km

The truth is in the gNB log: every "Emulated NTN channel updated ... rx_delay=Nus drift=D us/s" sets the delay
line to N and lets it run at D, so the round trip at time t is N + D (t - t_update). The instant of a round is
the SRS it was measured on: the gNB's own "[sfn.slot] SRS:" line for that SFN, nearest in time to the LMF line.

    scripts/ntn_multirtt_check.py /tmp/gnb_leo_nrppa.log            # LMF log from `docker logs oai-lmf`
    scripts/ntn_multirtt_check.py /tmp/gnb_leo_nrppa.log --lmf lmf.log --lmf-utc-offset 2
    scripts/ntn_multirtt_check.py /tmp/gnb_dualsat_sat1.log /tmp/gnb_dualsat_sat2.log

With two satellites serving at once there is one gNB log each: give them in the order of the OAM satellite ids
(satellite 0 first), and each satellite's solver input is scored against its own gNB's emulated channel.

The gNB log is root-owned: run with sudo, or copy it readable first.
"""
import argparse, bisect, math, re, statistics, subprocess, sys
from datetime import datetime, timedelta, timezone

C = 299792458.0
UE_TRUTH = (0.0, 0.0, 6356752.314245)  # the testbed's UE: the North Pole on WGS84 (gNB reference_location)
LMF = re.compile(r"^\[(\d{4}-\d\d-\d\d \d\d:\d\d:\d\d\.\d+)\].*Multi-RTT: UE Rx-Tx ([\d.]+) us \(offset (\d+) ms.*"
                 r"gNB Rx-Tx ([-+][\d.]+) us at sfn (\d+) \(PRS slot (\d+), SRS slot (\d+)\) = RTT ([\d.]+) us -> ([\d.]+) km")
UPDATE = re.compile(r"^(\S+) .*Emulated NTN channel updated.*rx_delay=([\d.]+)us drift=([-0-9.]+)")
SRS = re.compile(r"^(\S+) .*\[\s*(\d+)\.(\d+)\] SRS: ")
# Phase 4 LMF: each timed round, as the solver sees it (time from the SFN Initialisation Time, SRS-aligned range).
ROUND = re.compile(r"NTN Multi-RTT round \S+: t ([\d.]+) .*-> range ([\d.]+) m, satellite (\d+) at "
                   r"([-\d.]+) ([-\d.]+) ([-\d.]+)")


def utc(stamp):
    return datetime.fromisoformat(stamp).replace(tzinfo=timezone.utc).timestamp()


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("gnb_log", nargs="+", help="one per satellite, in OAM satellite id order")
    ap.add_argument("--lmf", help="LMF log file; default `docker logs oai-lmf`")
    ap.add_argument("--lmf-utc-offset", type=float,
                    help="hours the LMF's log clock is ahead of UTC; default: asked from the oai-lmf container")
    args = ap.parse_args()

    if args.lmf:
        lmf_lines = open(args.lmf, errors="replace").read().splitlines()
    else:
        out = subprocess.run(["docker", "logs", "oai-lmf"], capture_output=True, text=True, errors="replace")
        lmf_lines = (out.stdout + out.stderr).splitlines()
    offset_h = args.lmf_utc_offset
    if offset_h is None:
        z = subprocess.run(["docker", "exec", "oai-lmf", "date", "+%z"], capture_output=True, text=True).stdout.strip()
        offset_h = (int(z[:3]) + (1 if z[0] != "-" else -1) * int(z[3:]) / 60) if z else 0.0

    # One emulated-channel history per satellite: the i-th gNB log flies OAM satellite i.
    per_sat, srs = [], {}
    for sat_id, path in enumerate(args.gnb_log):
        try:
            gnb = open(path, errors="replace")
        except PermissionError:
            sys.exit(f"{path} is not readable - the gNB runs as root, so try sudo")
        ups = []
        for line in gnb:
            m = UPDATE.match(line)
            if m:
                ups.append((utc(m[1]), float(m[2]), float(m[3])))
                continue
            m = SRS.match(line)
            if m and m[3] == "0" and sat_id == 0:   # the SRS is the serving cell's
                srs.setdefault(int(m[2]), []).append(utc(m[1]))
        if not ups:
            sys.exit(f"no 'Emulated NTN channel updated' lines in {path}")
        per_sat.append(ups)
    updates = per_sat[0]
    times = [u[0] for u in updates]

    rows = []
    for line in lmf_lines:
        m = LMF.match(line)
        if not m:
            continue
        t_lmf = (datetime.fromisoformat(m[1]) - timedelta(hours=offset_h)).replace(tzinfo=timezone.utc).timestamp()
        cands = [t for t in srs.get(int(m[5]), []) if abs(t - t_lmf) < 5]
        if not cands:
            continue
        t = min(cands, key=lambda x: abs(x - t_lmf))
        k = bisect.bisect_right(times, t) - 1
        if k < 0:
            continue
        t0, d0, drift = updates[k]
        truth = d0 + drift * (t - t0)
        rtt = float(m[8])
        rows.append((t, rtt, truth, rtt - truth, float(m[2]), float(m[4]), int(m[3]), int(m[6]) - int(m[7])))

    if not rows:
        sys.exit("no LMF Multi-RTT round matched an SRS in this gNB log (is it the same run?)")
    # The UE half describes its UL subframe PRS subframe + offset (TS 38.215 5.1.46), the gNB half the SRS's:
    # align them at the rate the round trip moves between rounds, as the LMF does since phase 4.
    aligned = []
    for i, r in enumerate(rows):
        if i == 0 or r[0] - rows[i - 1][0] > 30:
            continue
        rate = (r[1] - rows[i - 1][1]) / (r[0] - rows[i - 1][0])
        aligned.append(r[1] - rate * (r[7] + r[6]) * 1e-3 - r[2])
    err = [r[3] for r in rows]
    print(f"{len(rows)} Multi-RTT rounds matched to the gNB's emulated round trip")
    print(f"  RTT - truth: mean {statistics.mean(err):+.3f} us  sd {statistics.pstdev(err):.3f} us   "
          f"=> range error mean {statistics.mean(err) * 1e-6 * C / 2:+.0f} m, sd {statistics.pstdev(err) * 1e-6 * C / 2:.0f} m")
    if aligned:
        print(f"  aligned to the SRS's UL subframe: mean {statistics.mean(aligned):+.3f} us  sd {statistics.pstdev(aligned):.3f} us"
              f"   => range error mean {statistics.mean(aligned) * 1e-6 * C / 2:+.0f} m, sd {statistics.pstdev(aligned) * 1e-6 * C / 2:.0f} m")
    # The solver's input: range at the LMF's own time tag against the emulated truth at that wall time, and the
    # satellite model at that tag against the same truth. The first is the measurement; the second is how exactly
    # the emulator follows the orbit (whole-microsecond rounding here was 57 m sd before it carried sub-us delays).
    solver = []
    for line in lmf_lines:
        m = ROUND.search(line)
        if not m:
            continue
        t, sat_id = float(m[1]), int(m[3])
        ups = per_sat[sat_id] if sat_id < len(per_sat) else None
        if ups is None:
            continue                      # no gNB log given for that satellite
        ts = [u[0] for u in ups]
        k = bisect.bisect_right(ts, t) - 1
        if k < 0:
            continue
        t0, d0, drift = ups[k]
        truth_m = (d0 + drift * (t - t0)) * 1e-6 * C / 2
        model_m = math.dist((float(m[4]), float(m[5]), float(m[6])), UE_TRUTH)
        solver.append((sat_id, float(m[2]) - truth_m, model_m - truth_m))
    if solver:
        for sat in sorted({x[0] for x in solver}):   # per satellite: a switch changes which orbit is modelled
            rows_s = [x for x in solver if x[0] == sat]
            a, b = [x[1] for x in rows_s], [x[2] for x in rows_s]
            print(f"  LMF solver input, satellite {sat} ({len(rows_s)} rounds): range - truth {statistics.mean(a):+.1f} m "
                  f"sd {statistics.pstdev(a):.1f} m; orbit model - emulated truth {statistics.mean(b):+.1f} m "
                  f"sd {statistics.pstdev(b):.1f} m")
    for r in rows[:: max(1, len(rows) // 10)]:
        print(f"  {datetime.fromtimestamp(r[0], timezone.utc):%H:%M:%S.%f}"[:-3] +
              f"  UE {r[4]:10.3f} + gNB {r[5]:+8.3f} = {r[1]:10.3f} us   truth {r[2]:10.3f}   "
              f"err {r[3]:+.3f} us   range {r[1] * 1e-6 * C / 2 / 1e3:8.3f} km")


if __name__ == "__main__":
    main()
