#!/usr/bin/env python3
"""Network-verified UE location for NTN, from the gNB's own log. Rel-18 style, TS 38.305.

WHAT IS BEING VERIFIED. A Rel-17 NTN UE pre-compensates the whole link round trip itself, from the SIB19
ephemeris and its OWN idea of where it is (for the OAI UE: the `position0` section of its config file, in ECEF
metres - it never reads SIB19's referenceLocation). If that position is wrong - bad GNSS, or a UE lying to get
service in another jurisdiction - the UE transmits at the wrong instant, and the network has to make up the
difference. So the evidence of a false position claim is not in any single measurement but in HOW MUCH THE
NETWORK HAD TO CORRECT:

    implied slant-range error = (c/2) * ( net commanded TA + leftover UL-RTOA residual )

The commanded part is the honest signal. The residual is mostly the noise floor of the cancellation (+-400 ns
here, about +-60 m) plus a fixed bias, so it is reported separately and only worth using after calibration.

WHY NOT THE OTHER OBSERVABLES, both measured on this testbed and rejected:
  - The UL-RTOA residual alone. The closed-loop TA drives it back to ta_target, so a static claim error largely
    vanishes from it: a 100 m lie moved the residual mean by 29 ns, where the geometry says 667 ns.
  - The PRACH-derived TA. Its bin is about 0.78 us (format 1, 1.25 kHz SCS), coarser than the errors of
    interest, and the logs carry plenty of false detections with detection_metric between 1 and 50 reporting
    hundreds of microseconds.

RESOLUTION AND RANGE. One TA command step is 16*64*Tc/2^mu = 0.52 us at 15 kHz, i.e. 78 m of range, and that is
the resolution of the whole method. Measured against deliberate claim errors: honest UE 0 m, a +100 m claim read
-78 m, a -100 m claim read +78 m - correct in sign and within one step. Upper limits: past roughly 300 m the SRS
stops being received at all (it sits on the last symbol of the slot and an early arrival pushes it out of the
estimator's window), and past about 1 km the UE cannot complete random access. A UE lying by kilometres cannot
use the cell at all, which is a coarse detector in itself. All of this is far finer than the 5-10 km consistency
check Rel-18 actually asks for.

    scripts/ntn_verify_position.py /tmp/gnb_leo_nrppa.log
    scripts/ntn_verify_position.py /tmp/gnb_claim_p100.log --expect -100
    scripts/ntn_verify_position.py /tmp/gnb_x.log --calib-ns 180 --threshold-m 5000

--calib-ns subtracts the residual bias an honest run shows on the same testbed (the control run read +180 ns);
it is the same idea as the LMF's LMF_TRP_DELAY_NS per-TRP constant.

LIMITATION, on purpose. This reads the gNB log because nothing carries the accumulated TA to the LMF: NRPPa has
no IE for it, and the OAI UE reports neither its TA nor its position (the Timing Advance Report MAC CE LCID is
defined and never used, and openair3/LPP holds only generated ASN.1). Moving this check into the LMF needs that
value plumbed out of the gNB first.
"""
import argparse, re, statistics, sys

C = 299792458.0
TA_STEP_US = 16 * 64 / (480000 * 4096) * 1e6 / 1  # 16*64*Tc at mu=0, in us -> 0.5208
SRS = re.compile(r"SRS: .*t_align=([+-][\d.]+)ns .*epre=(-?[\d.inf]+)dB")
TA_CMD = re.compile(r"TA_CMD: tag_id=\d+, ta_cmd=(\d+)")


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("gnb_log", help="the gNB log of the run (log.filename in the config)")
    ap.add_argument("--expect", type=float, metavar="M",
                    help="the claim error deliberately injected, in metres of slant range, to check against")
    ap.add_argument("--calib-ns", type=float, default=0.0,
                    help="residual bias of an honest run on this testbed, subtracted from the residual")
    ap.add_argument("--threshold-m", type=float, default=5000.0,
                    help="verdict threshold; Rel-18 asks for a 5-10 km consistency check (default 5000)")
    ap.add_argument("--min-coverage", type=float, default=0.2,
                    help="fraction of SRS occasions that must carry energy for a verdict at all (default 0.2)")
    ap.add_argument("--scs-khz", type=float, default=15.0, help="subcarrier spacing, for the TA step (default 15)")
    args = ap.parse_args()

    ta_step_us = TA_STEP_US / (args.scs_khz / 15.0)
    try:
        text = open(args.gnb_log, errors="replace").read()
    except PermissionError:
        sys.exit(f"{args.gnb_log} is not readable - the gNB runs as root, so try sudo")

    cmds = [int(m) for m in TA_CMD.findall(text)]
    # ta_cmd 31 means "no change"; the rest are signed steps around it (TS 38.213 4.2).
    net_steps = sum(c - 31 for c in cmds)
    net_ta_us = net_steps * ta_step_us

    live, silent = [], 0
    for align, epre in SRS.findall(text):
        if "inf" in epre:
            silent += 1
        else:
            live.append(float(align))

    print(f"gNB log: {args.gnb_log}")
    print(f"  TA commands:   {len(cmds)}  net {net_steps:+d} steps  = {net_ta_us:+.3f} us "
          f"(1 step = {ta_step_us:.4f} us = {ta_step_us * 1e-6 * C / 2:.0f} m)")
    if not live:
        print(f"  SRS:           none with energy ({silent} silent occasions)")
        print("  NOTE: no usable SRS. Past roughly 300 m of claim error the SRS leaves the estimator's window,")
        print("        so this alone is evidence the UE's position claim is wrong - see the module docstring.")
    else:
        mean = statistics.mean(live)
        sd = statistics.pstdev(live) if len(live) > 1 else 0.0
        print(f"  SRS:           {len(live)} with energy, {silent} silent   "
              f"residual mean {mean:+.0f} ns  sd {sd:.0f} ns")

    residual_ns = (statistics.mean(live) - args.calib_ns) if live else 0.0
    implied_ta_m = net_ta_us * 1e-6 * C / 2
    implied_all_m = (net_ta_us * 1e-6 + residual_ns * 1e-9) * C / 2

    print(f"\n  implied slant-range error, TA only:        {implied_ta_m:+.0f} m   <- the reliable figure")
    print(f"  implied slant-range error, TA + residual:   {implied_all_m:+.0f} m   "
          f"(residual {residual_ns:+.0f} ns after calibration)")

    if args.expect is not None:
        err = implied_ta_m - args.expect
        ok = abs(err) <= ta_step_us * 1e-6 * C / 2  # within one TA step
        print(f"  expected {args.expect:+.0f} m -> off by {err:+.0f} m: "
              f"{'OK, within one TA step' if ok else 'MISMATCH, more than one TA step'}")

    # Fail closed. Silence is not evidence of honesty: a claim error beyond a few hundred metres takes the SRS
    # out of the estimator's window entirely, which leaves net TA at zero and would otherwise read as a clean
    # "CONSISTENT" for the worst liar of the lot. Demand enough live measurements to have measured anything.
    coverage = len(live) / (len(live) + silent) if (live or silent) else 0.0
    if coverage < args.min_coverage:
        print(f"\n  verdict: UNVERIFIED - only {coverage:.1%} of SRS occasions carried any energy "
              f"(need {args.min_coverage:.0%})")
        print("  An SRS collapse is itself a symptom of a large claim error, not a clean run: the UE is")
        print("  transmitting far enough from its slot for the estimator to miss it. Treat as suspicious,")
        print("  cross-check the gNB's 'SRS:' epre values, and do not read the numbers above as a fix.")
        return 2
    verdict = "CONSISTENT" if abs(implied_ta_m) <= args.threshold_m else "INCONSISTENT"
    print(f"\n  verdict at a {args.threshold_m:.0f} m threshold: the UE's position claim is {verdict} "
          f"(SRS coverage {coverage:.0%})")
    return 0 if verdict == "CONSISTENT" else 1


if __name__ == "__main__":
    sys.exit(main())
