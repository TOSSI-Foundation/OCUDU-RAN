#!/usr/bin/env python3
"""Render the ray-traced route as a sequence of images, and optionally a video.

Produces one frame per receiver position showing the scene, the transmitter, the receiver
and the propagation paths, so the movement used by live_channel.py can be shown visually.

    source ~/sionna-rt-env/bin/activate
    python render_route.py --out-dir /tmp/route_frames \
        --rx-start 20 40 1.5 --rx-end 20 260 1.5 --steps 24

Combine the frames into a video with:

    ffmpeg -framerate 4 -i /tmp/route_frames/frame_%03d.png -pix_fmt yuv420p route.mp4
"""

import argparse
import json
import os
import sys

import numpy as np


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--out-dir", required=True)
    ap.add_argument("--scene", default="")
    ap.add_argument("--tx-position", type=float, nargs=3, default=[8.5, 21.0, 27.0])
    ap.add_argument("--rx-start", type=float, nargs=3, default=[20.0, 40.0, 1.5])
    ap.add_argument("--rx-end", type=float, nargs=3, default=[20.0, 260.0, 1.5])
    ap.add_argument("--steps", type=int, default=24)
    ap.add_argument("--max-depth", type=int, default=5)
    ap.add_argument("--resolution", type=int, nargs=2, default=[960, 720])
    ap.add_argument("--num-samples", type=int, default=128)
    ap.add_argument("--camera-position", type=float, nargs=3, default=[-150.0, 100.0, 180.0],
                    help="camera location; it looks at the middle of the route")
    args = ap.parse_args()

    import sionna.rt as rt
    from sionna.rt import (Camera, PathSolver, PlanarArray, Receiver, Transmitter,
                           load_scene)

    scene = load_scene(args.scene if args.scene else rt.scene.munich)
    scene.tx_array = PlanarArray(num_rows=1, num_cols=1, pattern="iso", polarization="V")
    scene.rx_array = PlanarArray(num_rows=1, num_cols=1, pattern="iso", polarization="V")
    scene.add(Transmitter(name="tx", position=args.tx_position))
    rx = Receiver(name="rx", position=args.rx_start)
    scene.add(rx)
    solver = PathSolver()

    start = np.array(args.rx_start, dtype=float)
    end = np.array(args.rx_end, dtype=float)
    middle = (start + end) / 2.0

    cam = Camera(position=args.camera_position, look_at=[float(middle[0]), float(middle[1]),
                                                         float(middle[2])])

    os.makedirs(args.out_dir, exist_ok=True)
    track = []

    for i in range(args.steps):
        frac = i / max(1, args.steps - 1)
        pos = start + frac * (end - start)
        rx.position = [float(pos[0]), float(pos[1]), float(pos[2])]

        paths = solver(scene, max_depth=args.max_depth)
        a, _ = paths.cir(out_type="numpy")
        a = np.squeeze(np.asarray(a))
        nof_paths = int((np.abs(a) > 1e-12).sum()) if a.size else 0
        gain_db = float(10.0 * np.log10(np.sum(np.abs(a) ** 2) + 1e-30))

        frame = os.path.join(args.out_dir, f"frame_{i:03d}.png")
        scene.render_to_file(camera=cam,
                             filename=frame,
                             paths=paths if nof_paths else None,
                             show_devices=True,
                             resolution=tuple(args.resolution),
                             num_samples=args.num_samples)

        track.append({"step": i, "position": [float(v) for v in pos],
                      "path_gain_db": gain_db, "nof_paths": nof_paths})
        print(f"[render] {i + 1}/{args.steps}  pos=({pos[0]:.0f},{pos[1]:.0f})  "
              f"gain={gain_db:7.1f}dB  paths={nof_paths:2d}  -> {os.path.basename(frame)}",
              flush=True)

    with open(os.path.join(args.out_dir, "track.json"), "w", encoding="utf-8") as handle:
        json.dump(track, handle, indent=2)

    print(f"\n[render] {args.steps} frames in {args.out_dir}")
    print(f"[render] make a video with:\n"
          f"  ffmpeg -y -framerate 4 -i {args.out_dir}/frame_%03d.png "
          f"-pix_fmt yuv420p {args.out_dir}/route.mp4")
    return 0


if __name__ == "__main__":
    sys.exit(main())
