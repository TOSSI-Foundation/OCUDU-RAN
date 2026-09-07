#!/usr/bin/env bash
#
# run_stack.sh — launch the OCUDU-RAN stack (CU then DU).
#
# Starts the CU, waits for it to come up, then starts the DU pinned to cores 1-6.
# Ctrl-C stops both cleanly. Logs go to logs/<timestamp>/.
#
#   ./run_stack.sh                        # default DU config (csi_ml_example.yaml)
#   ./run_stack.sh <du-config.yaml>       # override the DU config
#   ./run_stack.sh -h                     # help
#
# Note: run as the SAME user each time (mixing root/normal user leaves
# root-owned files that later runs cannot overwrite).

set -uo pipefail

REPO="${REPO:-$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)}"
CU_BIN="$REPO/build/apps/cu/ocu"
DU_BIN="$REPO/build/apps/du/odu"
CU_CFG="$REPO/configs/cu.yml"
DU_CFG="${1:-$REPO/configs/csi_ml_example.yaml}"
# 1-10 = the isolated cores (isolcpus). The YAML assigns them per thread; CPU 0 must NEVER be
# included (housekeeping core -- an RT thread there starves the NIC and freezes the server).
# The CU is deliberately NOT tasksetted: unpinned processes cannot land on isolated cores.
DU_CORES="1-10"
CU_WAIT_SECS=8

if [[ "${1:-}" == "-h" || "${1:-}" == "--help" ]]; then
    awk 'NR>1 { if (/^#/) { sub(/^# ?/,""); print } else exit }' "$0"
    exit 0
fi

RUN_DIR="$REPO/logs/$(date +%Y%m%d_%H%M%S)"
CU_LOG="$RUN_DIR/cu.log"
DU_LOG="$RUN_DIR/du.log"

CU_PID=""
DU_PID=""

cleanup() {
    echo
    echo "[stack] shutting down..."
    # DU first: it is the CU's client, so stopping it first avoids F1 teardown errors.
    for name_pid in "DU:$DU_PID" "CU:$CU_PID"; do
        name="${name_pid%%:*}"; pid="${name_pid#*:}"
        if [[ -n "$pid" ]] && kill -0 "$pid" 2>/dev/null; then
            echo "[stack] stopping $name (pid $pid)"
            kill -TERM "$pid" 2>/dev/null
            for _ in $(seq 1 10); do
                kill -0 "$pid" 2>/dev/null || break
                sleep 0.5
            done
            kill -0 "$pid" 2>/dev/null && { echo "[stack] $name did not exit, killing"; kill -KILL "$pid" 2>/dev/null; }
        fi
    done
    echo "[stack] logs: $RUN_DIR"
    exit 0
}
trap cleanup INT TERM

# ---- preflight ----
for f in "$CU_BIN" "$DU_BIN" "$CU_CFG" "$DU_CFG"; do
    [[ -e "$f" ]] || { echo "[stack] ERROR: missing $f" >&2; exit 1; }
done
[[ -x "$CU_BIN" ]] || { echo "[stack] ERROR: $CU_BIN not executable" >&2; exit 1; }
[[ -x "$DU_BIN" ]] || { echo "[stack] ERROR: $DU_BIN not executable" >&2; exit 1; }

mkdir -p "$RUN_DIR"

echo "[stack] user      : $(whoami)"
echo "[stack] DU config : $DU_CFG"
echo "[stack] logs      : $RUN_DIR"

# Surface the CSI-ML settings so the run is self-documenting: which model is
# active, whether it actuates, and the scenario tag the CSVs will carry.
if grep -qE '^\s*csi_ml:' "$DU_CFG"; then
    echo "[stack] csi_ml:"
    sed -n '/^\s*csi_ml:/,/^[^[:space:]#]/p' "$DU_CFG" \
        | grep -E '^\s+(enabled|model_type|apply_to_mcs|scenario|output_dir|gru_model_path|wiener_model_path):' \
        | sed 's/^/           /'
fi
echo

# ---- CU ----
echo "[stack] starting CU..."
( cd "$(dirname "$CU_BIN")" && exec "$CU_BIN" -c "$CU_CFG" ) >"$CU_LOG" 2>&1 &
CU_PID=$!
sleep "$CU_WAIT_SECS"
if ! kill -0 "$CU_PID" 2>/dev/null; then
    echo "[stack] ERROR: CU exited during startup. Last lines:" >&2
    tail -25 "$CU_LOG" >&2
    exit 1
fi
echo "[stack] CU up (pid $CU_PID)"

# ---- DU ----
echo "[stack] starting DU on cores $DU_CORES..."
( cd "$(dirname "$DU_BIN")" && exec taskset -c "$DU_CORES" "$DU_BIN" -c "$DU_CFG" ) >"$DU_LOG" 2>&1 &
DU_PID=$!
sleep 3
if ! kill -0 "$DU_PID" 2>/dev/null; then
    echo "[stack] ERROR: DU exited during startup. Last lines:" >&2
    tail -25 "$DU_LOG" >&2
    cleanup
fi
echo "[stack] DU up (pid $DU_PID)"
echo
echo "[stack] running — Ctrl-C to stop both. Following DU log:"
echo "------------------------------------------------------------"

tail -f "$DU_LOG" &
TAIL_PID=$!

# Exit as soon as either process dies, so a crash is not silently ignored.
while kill -0 "$CU_PID" 2>/dev/null && kill -0 "$DU_PID" 2>/dev/null; do
    sleep 1
done

kill "$TAIL_PID" 2>/dev/null
kill -0 "$CU_PID" 2>/dev/null || echo "[stack] CU exited unexpectedly — see $CU_LOG"
kill -0 "$DU_PID" 2>/dev/null || echo "[stack] DU exited unexpectedly — see $DU_LOG"
cleanup
