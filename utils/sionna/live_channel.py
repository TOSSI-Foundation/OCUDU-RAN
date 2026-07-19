#!/usr/bin/env python3
"""Stream a Sionna RT ray-traced channel to a running OCUDU DU.

Publishes channel updates on a ZeroMQ socket that the OCUDU `sionna` radio driver
subscribes to, so the channel changes while the RAN keeps running. Launch the DU with

    device_args: ...,live=tcp://127.0.0.1:5566

Modes:
  --route      move the receiver along a straight route, re-tracing at each step
  --static     trace one position and publish it repeatedly
  --sweep      publish a synthetic attenuation ramp, no ray tracing (fast demo)

Ray tracing takes a few hundred milliseconds per position, so updates arrive at a few
hertz. The RAN never waits for them: it keeps transmitting with the last channel it
received and swaps in the new one between blocks.
"""

import argparse
import struct
import sys
import time

import numpy as np
import zmq

MAGIC = 0x4F434952
VERSION = 1
HEADER = struct.Struct("<8I2d")


def encode(taps, fs_hz, sequence, snapshot_dt_s=0.0, loop=False):
    """Packs taps shaped [snapshot][rx_ant][tx_ant][tap] into a channel update."""
    taps = np.ascontiguousarray(taps, dtype=np.complex64)
    ns, nrx, ntx, nt = taps.shape
    header = HEADER.pack(MAGIC, VERSION, sequence & 0xFFFFFFFF, ns, ntx, nrx, nt,
                         1 if loop else 0, fs_hz, snapshot_dt_s)
    return header + taps.tobytes()


class Tracer:
    """Ray-traces the scene for a given receiver position."""

    def __init__(self, args):
        import sionna.rt as rt
        from sionna.rt import (PathSolver, PlanarArray, Receiver, Transmitter,
                               load_scene)
        self.args = args
        self.scene = load_scene(args.scene if args.scene else rt.scene.munich)
        self.scene.tx_array = PlanarArray(num_rows=1, num_cols=args.num_tx_ant,
                                          pattern="iso", polarization="V")
        self.scene.rx_array = PlanarArray(num_rows=1, num_cols=args.num_rx_ant,
                                          pattern="iso", polarization="V")
        self.scene.add(Transmitter(name="tx", position=args.tx_position))
        self.rx = Receiver(name="rx", position=args.rx_start)
        self.scene.add(self.rx)
        self.solver = PathSolver()
        self.anchor_db = None

    def trace(self, position):
        """Returns (taps[1,rx,tx,taps], path_gain_dB, nof_paths) for a position."""
        self.rx.position = [float(position[0]), float(position[1]), float(position[2])]
        paths = self.solver(self.scene, max_depth=self.args.max_depth)

        a, _ = paths.cir(out_type="numpy")
        a = np.squeeze(np.asarray(a))
        nof_paths = int((np.abs(a) > 1e-12).sum()) if a.size else 0
        gain_db = 10.0 * np.log10(np.sum(np.abs(a) ** 2) + 1e-30)

        shape = paths.taps(bandwidth=self.args.srate, l_min=0, l_max=self.args.num_taps - 1,
                           sampling_frequency=self.args.srate, normalize=True,
                           normalize_delays=True, out_type="numpy")
        shape = np.squeeze(np.asarray(shape))

        block = np.zeros((1, self.args.num_rx_ant, self.args.num_tx_ant,
                          self.args.num_taps), dtype=np.complex64)
        flat = shape.reshape(-1)[:self.args.num_taps] if shape.size else np.zeros(0)
        block[0, 0, 0, :len(flat)] = flat

        # Reference the strongest position seen so far, then apply the real relative
        # loss, so the RAN sees the actual attenuation rather than a normalized channel.
        if gain_db > -200:
            self.anchor_db = gain_db if self.anchor_db is None else max(self.anchor_db, gain_db)
        drop = self.args.max_drop_db if gain_db <= -200 else \
            min(max(0.0, (self.anchor_db or gain_db) - gain_db), self.args.max_drop_db)
        block *= 10.0 ** (-drop / 20.0)
        return block, gain_db, nof_paths, drop


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--bind", default="tcp://127.0.0.1:5566",
                    help="ZeroMQ endpoint the DU subscribes to")
    ap.add_argument("--srate", type=float, default=61.44e6)
    ap.add_argument("--num-taps", type=int, default=16)
    ap.add_argument("--num-tx-ant", type=int, default=1)
    ap.add_argument("--num-rx-ant", type=int, default=1)
    ap.add_argument("--interval", type=float, default=1.0,
                    help="seconds between published updates")
    ap.add_argument("--scene", default="")
    ap.add_argument("--tx-position", type=float, nargs=3, default=[8.5, 21.0, 27.0])
    ap.add_argument("--rx-start", type=float, nargs=3, default=[20.0, 40.0, 1.5])
    ap.add_argument("--rx-end", type=float, nargs=3, default=[20.0, 330.0, 1.5])
    ap.add_argument("--steps", type=int, default=24, help="positions along the route")
    ap.add_argument("--max-depth", type=int, default=5)
    ap.add_argument("--max-drop-db", type=float, default=45.0,
                    help="clamp on the attenuation applied relative to the strongest point")
    ap.add_argument("--loop-route", action="store_true",
                    help="drive the route back and forth forever")
    mode = ap.add_mutually_exclusive_group()
    mode.add_argument("--route", action="store_true", help="drive along the route (default)")
    mode.add_argument("--static", action="store_true", help="one position, republished")
    mode.add_argument("--sweep", action="store_true",
                      help="synthetic attenuation ramp, no ray tracing")
    args = ap.parse_args()

    ctx = zmq.Context()
    sock = ctx.socket(zmq.PUB)
    sock.bind(args.bind)
    print(f"[live] publishing on {args.bind}", flush=True)
    # Give subscribers time to connect before the first message.
    time.sleep(0.5)

    seq = 0
    try:
        if args.sweep:
            print("[live] synthetic attenuation sweep (no ray tracing)", flush=True)
            while True:
                for drop in list(range(0, 51, 5)) + list(range(45, -1, -5)):
                    taps = np.zeros((1, args.num_rx_ant, args.num_tx_ant,
                                     args.num_taps), dtype=np.complex64)
                    taps[0, :, :, 0] = 10.0 ** (-drop / 20.0)
                    sock.send(encode(taps, args.srate, seq))
                    print(f"[live] seq={seq:4d}  attenuation={drop:3d} dB", flush=True)
                    seq += 1
                    time.sleep(args.interval)

        tracer = Tracer(args)
        start = np.array(args.rx_start, dtype=float)
        end = np.array(args.rx_end, dtype=float)

        if args.static:
            positions = [start]
        else:
            positions = [start + (i / max(1, args.steps - 1)) * (end - start)
                         for i in range(args.steps)]

        idx, step = 0, 1
        while True:
            pos = positions[idx]
            t0 = time.time()
            taps, gain_db, nof_paths, drop = tracer.trace(pos)
            trace_ms = (time.time() - t0) * 1e3
            sock.send(encode(taps, args.srate, seq))
            print(f"[live] seq={seq:4d}  pos=({pos[0]:.0f},{pos[1]:.0f})  "
                  f"gain={gain_db:7.1f}dB  applied=-{drop:4.1f}dB  paths={nof_paths:2d}  "
                  f"trace={trace_ms:5.0f}ms", flush=True)
            seq += 1

            if len(positions) > 1:
                idx += step
                if idx >= len(positions):
                    if args.loop_route:
                        idx, step = len(positions) - 2, -1
                    else:
                        idx = 0
                elif idx < 0:
                    idx, step = 1, 1
            time.sleep(max(0.0, args.interval - (time.time() - t0)))

    except KeyboardInterrupt:
        print("\n[live] stopped", flush=True)
    finally:
        sock.close()
        ctx.term()
    return 0


if __name__ == "__main__":
    sys.exit(main())
