#!/bin/bash
# Regenerates the CIR artifacts used by the Sionna channel configs.
#
#   source ~/sionna-rt-env/bin/activate
#   bash utils/sionna/make_artifacts.sh
#
# Writes into <repo>/artifacts. The ray-traced set needs the Sionna environment; the
# synthetic ones do not.
set -eu

HERE=$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)
OUT=$HERE/artifacts
SRATE=61.44e6
TAPS=16

mkdir -p "$OUT"
echo "==> writing artifacts to $OUT"

# Unit impulse: output equals input. Reference channel and a safe startup channel.
python3 "$HERE/utils/sionna/export_cir.py" --out-dir "$OUT/cir_pass" \
    --srate $SRATE --num-taps $TAPS --synthetic passthrough

# Strong line of sight with absolute normalization, used as the DU startup channel.
python3 - "$OUT/cir_strong" $SRATE $TAPS <<'PY'
import json, os, sys
import numpy as np
out, srate, taps = sys.argv[1], float(sys.argv[2]), int(sys.argv[3])
os.makedirs(out, exist_ok=True)
t = np.zeros((1, 1, 1, taps), dtype=np.complex64)
t[0, 0, 0, 0] = 1.0
t.tofile(f"{out}/cir.bin")
json.dump({"format": "ocudu-sionna-cir", "version": 1, "scene": "strong_los_0dB",
           "fs_hz": srate, "snapshot_dt_s": 0.0, "num_snapshots": 1, "num_tx_ant": 1,
           "num_rx_ant": 1, "num_taps": taps, "normalization": "absolute",
           "loop": False, "data_file": "cir.bin"},
          open(f"{out}/manifest.json", "w"), indent=2)
print(f"[export] wrote {out}/manifest.json (strong line of sight)")
PY

# Ray-traced Munich channel. Skipped when Sionna is unavailable.
if python3 -c "import sionna.rt" 2>/dev/null; then
    python3 "$HERE/utils/sionna/export_cir.py" --out-dir "$OUT/cir_munich" \
        --srate $SRATE --num-taps $TAPS \
        --tx-position 8.5 21 27 --rx-start 45 90 1.5
else
    echo "[export] Sionna not importable, skipping the ray-traced artifact."
    echo "         Activate the environment first: source ~/sionna-rt-env/bin/activate"
fi

echo "==> done"
ls -1 "$OUT"
