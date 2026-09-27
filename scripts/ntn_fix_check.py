#!/usr/bin/env python3
"""Score the LMF's single-satellite NTN Multi-RTT fixes (TS 38.305 8.10) against the truth and the UE's claim.

The LMF logs one line per location session once it has enough rounds:

    NTN Multi-RTT fix imsi-...: lat 89.997409 lon 12.3, 1-sigma 1157 x 3 m (major axis 12 deg from north), range rms
    19.7 m, 37 rounds over 180 s

Truth: where the emulation puts the UE, the gNB's reference_location (this testbed: the North Pole, height 0).
Claim: where the UE says it is - for the OAI UE, position0 (ECEF) in its config file, which is what it
pre-compensates its timing advance from. Rel-18 network verification of UE location asks whether the claim is
consistent with the network's own fix; its accuracy target is 5-10 km.

    scripts/ntn_fix_check.py                                   # docker logs oai-lmf, truth = North Pole
    scripts/ntn_fix_check.py --ue-conf path/to/ue.conf         # also check the UE's claimed position0
    scripts/ntn_fix_check.py --truth 90,0 --threshold-km 5
"""
import argparse, math, re, subprocess, sys

A, B = 6378137.0, 6356752.314245
FIX = re.compile(r"NTN Multi-RTT fix \S+: lat ([-\d.]+) lon ([-\d.]+), 1-sigma (\d+) x (\d+) m .*range rms ([\d.]+) m, "
                 r"(\d+) rounds over (\d+) s( AMBIGUOUS)?")


def ecef(lat, lon, h=0.0):
    lat, lon = math.radians(lat), math.radians(lon)
    e2 = 1 - B * B / (A * A)
    n = A / math.sqrt(1 - e2 * math.sin(lat) ** 2)
    return ((n + h) * math.cos(lat) * math.cos(lon), (n + h) * math.cos(lat) * math.sin(lon),
            (n * (1 - e2) + h) * math.sin(lat))


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--lmf", help="LMF log file; default `docker logs oai-lmf`")
    ap.add_argument("--truth", default="90,0", help="true UE lat,lon (default the testbed's North Pole)")
    ap.add_argument("--ue-conf", help="OAI UE config: its position0 is the UE's claimed position")
    ap.add_argument("--threshold-km", type=float, default=10.0, help="verification threshold (Rel-18: 5-10 km)")
    args = ap.parse_args()

    text = open(args.lmf).read() if args.lmf else subprocess.run(
        ["docker", "logs", "oai-lmf"], capture_output=True, text=True, errors="replace").stdout
    fixes = [tuple(float(x) for x in m.groups()[:7]) + (bool(m[8]),) for m in FIX.finditer(text)]
    if not fixes:
        sys.exit("no 'NTN Multi-RTT fix' lines - not enough rounds yet, or no OAM satellite information")
    truth = ecef(*(float(v) for v in args.truth.split(",")))
    claim = None
    if args.ue_conf:
        conf = open(args.ue_conf).read()
        p = re.search(r"position0\s*=\s*\{([^}]*)\}", conf)
        if p:
            claim = tuple(float(re.search(rf"\b{k}\s*=\s*([-\d.eE+]+)", p[1])[1]) for k in "xyz")

    print(f"{len(fixes)} fixes; truth {args.truth}" + (f"; UE claim position0 {claim}" if claim else ""))
    for lat, lon, a, b, rms, n, span, amb in fixes[:: max(1, len(fixes) // 12)] + ([fixes[-1]] if len(fixes) > 12 else []):
        f = ecef(lat, lon)
        err = math.dist(f, truth)
        line = (f"  {int(n):4d} rounds / {int(span):4d} s: error {err:8.1f} m   1-sigma {a:6.0f} x {b:4.0f} m   "
                f"({err / a if a else float('inf'):.1f} sigma)   range rms {rms:5.1f} m")
        if amb:
            line += "   AMBIGUOUS (LMF: another point fits nearly as well - no verdict)"
        elif claim:
            d = math.dist(f, claim) / 1e3
            line += f"   claim {d:6.2f} km: {'CONSISTENT' if d <= args.threshold_km else 'INCONSISTENT'}"
        print(line)


if __name__ == "__main__":
    main()
