#!/usr/bin/env python3
"""Export a Sionna RT ray-traced channel to an OCUDU CIR artifact.

Produces the manifest/binary pair consumed by the OCUDU `sionna` radio driver:

    manifest.json   geometry, sampling rate and normalization metadata
    cir.bin         [snapshot][rx_ant][tx_ant][tap] complex64, little endian

Run inside the Sionna RT environment, e.g.

    source ~/sionna-rt-env/bin/activate
    python export_cir.py --out-dir /tmp/cir_munich --srate 61.44e6 --num-taps 16

Use --synthetic to generate an artifact without ray tracing, which is useful to
validate the RAN integration independently of the scene setup.
"""

import argparse
import json
import os
import sys

import numpy as np

MANIFEST_FORMAT = "ocudu-sionna-cir"
MANIFEST_VERSION = 1


def trace_snapshots(args):
    """Ray-traces the scene and returns taps shaped [snapshot][rx_ant][tx_ant][tap]."""
    import sionna.rt as rt
    from sionna.rt import (PathSolver, PlanarArray, Receiver, Transmitter,
                           load_scene)

    scene = load_scene(args.scene if args.scene else rt.scene.munich)
    scene.tx_array = PlanarArray(num_rows=1, num_cols=args.num_tx_ant,
                                 pattern="iso", polarization="V")
    scene.rx_array = PlanarArray(num_rows=1, num_cols=args.num_rx_ant,
                                 pattern="iso", polarization="V")
    scene.add(Transmitter(name="tx", position=args.tx_position))
    rx = Receiver(name="rx", position=args.rx_start)
    scene.add(rx)

    solver = PathSolver()
    start = np.array(args.rx_start, dtype=float)
    end = np.array(args.rx_end if args.rx_end else args.rx_start, dtype=float)

    snapshots = []
    for idx in range(args.num_snapshots):
        if args.num_snapshots > 1:
            frac = idx / (args.num_snapshots - 1)
        else:
            frac = 0.0
        pos = start + frac * (end - start)
        rx.position = [float(pos[0]), float(pos[1]), float(pos[2])]

        paths = solver(scene, max_depth=args.max_depth)
        taps = paths.taps(bandwidth=args.srate,
                          l_min=0,
                          l_max=args.num_taps - 1,
                          sampling_frequency=args.srate,
                          normalize=True,
                          normalize_delays=True,
                          out_type="numpy")
        taps = np.asarray(taps)
        # Collapse the leading rx/tx object dimensions, keeping antennas and taps.
        taps = np.squeeze(taps)
        block = np.zeros((args.num_rx_ant, args.num_tx_ant, args.num_taps),
                         dtype=np.complex64)
        if taps.ndim == 1:
            block[0, 0, :min(args.num_taps, taps.shape[-1])] = \
                taps[:min(args.num_taps, taps.shape[-1])]
        elif taps.ndim == 2:
            rr = min(args.num_rx_ant, taps.shape[0])
            ll = min(args.num_taps, taps.shape[-1])
            block[:rr, 0, :ll] = taps[:rr, :ll]
        else:
            rr = min(args.num_rx_ant, taps.shape[0])
            tt = min(args.num_tx_ant, taps.shape[1])
            ll = min(args.num_taps, taps.shape[-1])
            block[:rr, :tt, :ll] = taps[:rr, :tt, :ll]

        energy = float(np.sum(np.abs(block) ** 2))
        print(f"[export] snapshot {idx}: rx={rx.position} energy={energy:.4f}",
              flush=True)
        snapshots.append(block)

    return np.stack(snapshots, axis=0)


def synthetic_snapshots(args):
    """Builds a deterministic artifact that does not require ray tracing."""
    taps = np.zeros((args.num_snapshots, args.num_rx_ant, args.num_tx_ant,
                     args.num_taps), dtype=np.complex64)
    if args.synthetic == "passthrough":
        taps[:, :, :, 0] = 1.0
    else:
        # Two-tap profile with a decaying echo, scaled to unit energy.
        taps[:, :, :, 0] = 0.9
        if args.num_taps > 1:
            taps[:, :, :, min(1, args.num_taps - 1)] = 0.4358899
    return taps


def main():
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--out-dir", required=True)
    parser.add_argument("--srate", type=float, default=61.44e6,
                        help="sampling rate in Hz, must match the RU srate")
    parser.add_argument("--num-taps", type=int, default=16)
    parser.add_argument("--num-tx-ant", type=int, default=1)
    parser.add_argument("--num-rx-ant", type=int, default=1)
    parser.add_argument("--num-snapshots", type=int, default=1)
    parser.add_argument("--snapshot-dt", type=float, default=0.1,
                        help="seconds between snapshots")
    parser.add_argument("--loop", action="store_true",
                        help="restart the timeline after the last snapshot")
    parser.add_argument("--scene", default="",
                        help="Mitsuba scene XML, defaults to the bundled Munich scene")
    parser.add_argument("--tx-position", type=float, nargs=3,
                        default=[8.5, 21.0, 27.0])
    parser.add_argument("--rx-start", type=float, nargs=3,
                        default=[45.0, 90.0, 1.5])
    parser.add_argument("--rx-end", type=float, nargs=3, default=None)
    parser.add_argument("--max-depth", type=int, default=5)
    parser.add_argument("--synthetic", choices=["passthrough", "two_tap"],
                        default=None,
                        help="skip ray tracing and emit a deterministic artifact")
    args = parser.parse_args()

    if args.num_taps < 1 or args.num_taps > 64:
        parser.error("--num-taps must be in [1, 64]")
    if args.num_snapshots < 1:
        parser.error("--num-snapshots must be positive")

    if args.synthetic:
        taps = synthetic_snapshots(args)
        scene_id = f"synthetic:{args.synthetic}"
    else:
        taps = trace_snapshots(args)
        scene_id = args.scene if args.scene else "sionna.rt.scene.munich"

    os.makedirs(args.out_dir, exist_ok=True)
    data_name = "cir.bin"
    data_path = os.path.join(args.out_dir, data_name)
    taps.astype(np.complex64).tofile(data_path)

    manifest = {
        "format": MANIFEST_FORMAT,
        "version": MANIFEST_VERSION,
        "scene": scene_id,
        "fs_hz": args.srate,
        "snapshot_dt_s": args.snapshot_dt if args.num_snapshots > 1 else 0.0,
        "num_snapshots": args.num_snapshots,
        "num_tx_ant": args.num_tx_ant,
        "num_rx_ant": args.num_rx_ant,
        "num_taps": args.num_taps,
        "normalization": "unit_energy",
        "loop": bool(args.loop),
        "data_file": data_name,
    }
    manifest_path = os.path.join(args.out_dir, "manifest.json")
    with open(manifest_path, "w", encoding="utf-8") as handle:
        json.dump(manifest, handle, indent=2)
        handle.write("\n")

    print(f"[export] wrote {manifest_path} ({taps.nbytes} bytes of taps)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
