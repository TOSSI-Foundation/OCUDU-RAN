#!/usr/bin/env python3
"""Write the LMF's NTN satellite information (its OAM input, TS 38.305 5.4) from a gNB NTN config.

TS 38.305: "For NTN, the LMF is configured by the OAM with satellite related information ..., as well as the
association between TRP(s) and satellite(s)". In this testbed the gNB config is the source of truth for the
ephemeris and the run scripts re-stamp its epoch every run, so this stands in for OAM and runs right after the
stamp:

    scripts/lmf_ntn_oam.py configs/ntn/leo_rfsim_gnb.yml ~/oai-cn5g/conf/ntn/ntn_satellites.json [--gnb-id 411]
    scripts/lmf_ntn_oam.py configs/ntn/leo_rfsim_gnb.yml OUT --switch-cfg configs/ntn/leo_satswitch_xplane.yml
    scripts/lmf_ntn_oam.py configs/ntn/leo_rfsim_gnb.yml OUT --add configs/ntn/leo_dualsat_sat2.yml:412

With --switch-cfg the cell changes satellite at t_service (TS 38.331 satSwitchWithReSync-r18), so TWO satellites
are written, each with the window in which it carries the TRP: the LMF picks by the instant of each measurement.

With --add CFG:GNBID a satellite SERVING AT THE SAME TIME is added, carrying the TRP of another gNB, with no
window at all. That is the two-satellites-at-once case: the LMF then has two ranges per instant instead of one
per instant along a pass.

The LMF (oai-lmf, LMF_NTN_SATELLITES) re-reads the file when it changes. One TRP per cell, numbered from 1.
"""
import argparse, json, os, sys

import yaml


def ntn_of(path):
    cfg = yaml.safe_load(open(path))
    ntn = (cfg.get("cell_cfg") or {}).get("ntn")
    if not ntn:
        sys.exit(f"{path}: no cell_cfg.ntn block")
    return ntn


def state(eph):
    return ([float(eph[k]) for k in ("pos_x", "pos_y", "pos_z")],
            [float(eph[k]) for k in ("vel_x", "vel_y", "vel_z")])


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("gnb_cfg")
    ap.add_argument("out")
    ap.add_argument("--switch-cfg", help="a satellite-switch overlay: adds the second satellite and the windows")
    ap.add_argument("--add", action="append", default=[], metavar="CFG:GNBID",
                    help="another gNB's config and gnb_id: its satellite serves at the same time, no window")
    ap.add_argument("--gnb-id", type=int, default=411, help="gnb_id of the gNB serving through these satellites")
    ap.add_argument("--trp-id", type=int, default=1)
    ap.add_argument("--rtt-calibration-ns", type=float, default=0.0,
                    help="per-TRP RTT correction, subtracted before ranging (the NTN counterpart of LMF_TRP_DELAY_NS)")
    args = ap.parse_args()

    base = ntn_of(args.gnb_cfg)
    if base.get("feeder_link_info") or base.get("ta_common_offset"):
        sys.exit("a feeder link / ta-Common is configured: set commonDelayUs, this script assumes none")
    epoch = str(base["epoch_timestamp"]) + "Z"   # the run scripts stamp it with date -u
    trps = [{"gnbId": args.gnb_id, "trpId": args.trp_id}]

    def sat(idx, eph, serves_from=None, serves_until=None):
        pos, vel = state(eph)
        s = {"id": idx, "epoch": epoch, "ecefPositionM": pos, "ecefVelocityMS": vel, "trps": trps,
             "commonDelayUs": 0.0,  # no feeder link: the gNB's emulated delay is the service link alone
             "rttCalibrationNs": args.rtt_calibration_ns}
        if serves_from:
            s["servesFrom"] = serves_from
        if serves_until:
            s["servesUntil"] = serves_until
        return s

    sats = []
    if args.switch_cfg:
        sw_ntn = ntn_of(args.switch_cfg)
        sw = sw_ntn.get("sat_switch_with_resync")
        if not sw:
            sys.exit(f"{args.switch_cfg}: no cell_cfg.ntn.sat_switch_with_resync block")
        # The serving satellite hands the cell over at t_service; the second takes it from the same instant.
        handover = str(sw_ntn.get("t_service") or sw["t_service_start"]) + "Z"
        sats.append(sat(0, base["ephemeris_info_ecef"], serves_until=handover))
        sats.append(sat(1, sw["ephemeris_info_ecef"], serves_from=handover))
    else:
        sats.append(sat(0, base["ephemeris_info_ecef"]))

    # Satellites serving at the same time, each carrying its own gNB's TRP. The epoch is the base config's: the
    # run scripts stamp it once and every overlay inherits it, so all the orbits are one function of one clock.
    for spec in args.add:
        path, _, gnb_id = spec.rpartition(":")
        if not path or not gnb_id.isdigit():
            sys.exit(f"--add {spec}: expected CFG:GNBID")
        extra = ntn_of(path)
        if extra.get("epoch_timestamp") and str(extra["epoch_timestamp"]) + "Z" != epoch:
            sys.exit(f"{path}: epoch {extra['epoch_timestamp']} differs from {args.gnb_cfg}'s {epoch}")
        s = sat(len(sats), extra["ephemeris_info_ecef"])
        s["trps"] = [{"gnbId": int(gnb_id), "trpId": args.trp_id}]
        sats.append(s)

    tmp = args.out + ".tmp"
    with open(tmp, "w") as f:
        json.dump({"satellites": sats}, f, indent=2)
    os.replace(tmp, args.out)  # atomic: the LMF never reads half a file
    print(f"LMF NTN OAM: {args.out} epoch {epoch}, {len(sats)} satellite(s)" +
          (f", switch at {sats[1]['servesFrom']}" if len(sats) > 1 and "servesFrom" in sats[1] else "") +
          "".join(f", satellite {s['id']} on gnbId {s['trps'][0]['gnbId']}" for s in sats[1:]
                  if "servesFrom" not in s))


if __name__ == "__main__":
    main()
