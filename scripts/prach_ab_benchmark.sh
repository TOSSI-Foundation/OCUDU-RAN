#!/usr/bin/env bash

set -u

usage() {
  cat <<'EOF'
prach_ab_benchmark.sh — A/B system benchmark: inline-GPU PRACH vs default CPU PRACH.

Usage:
  sudo ./scripts/prach_ab_benchmark.sh [duration_s] [warmup_s] [out_dir]

Args (all optional, positional):
  duration_s   measured sampling window per mode, integer seconds   (default 30)
  warmup_s     warmup before sampling per mode, integer seconds     (default 12)
  out_dir      directory for logs + samples         (default /tmp/prach_ab_<ts>)

Prereq: ru_emulator must already be running and streaming, e.g.:
  sudo ./build_72/apps/examples/ofh/ru_emulator -c configs/emu.yml

The script restarts ONLY the gnb (twice: prach_rx_to_gpu true then false), parses
detector latency for each, and prints a comparison table.

It exports OCUDU_PRACH_BENCH=1 so the gnb emits the per-window detector stats this
table parses. Without that variable the gnb runs silent (no benchmark log spam),
which is the default for normal/interactive runs.
EOF
}

case "${1:-}" in
  -h|--help|help) usage; exit 0 ;;
esac

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BUILD_DIR="${REPO_ROOT}/build_72"
GNB_BIN="${BUILD_DIR}/apps/gnb/gnb"
GNB_YML="${REPO_ROOT}/configs/gpu_inline_acc_gnb.yml"

DURATION="${1:-30}"
WARMUP="${2:-12}"
OUT_DIR="${3:-/tmp/prach_ab_$(date +%Y%m%d_%H%M%S)}"

if ! [[ "$DURATION" =~ ^[0-9]+$ && "$WARMUP" =~ ^[0-9]+$ ]]; then
  echo "ERROR: duration_s and warmup_s must be non-negative integers (got '$DURATION' '$WARMUP')." >&2
  echo "Run with --help for usage." >&2
  exit 1
fi

if [[ $EUID -ne 0 ]]; then
  echo "ERROR: must run as root (gnb binds a DPDK port). Re-run with sudo." >&2
  exit 1
fi
for f in "$GNB_BIN" "$GNB_YML"; do
  [[ -e "$f" ]] || { echo "ERROR: missing $f" >&2; exit 1; }
done
if ! pgrep -f "apps/examples/ofh/ru_emulator" >/dev/null; then
  echo "ERROR: ru_emulator is not running. Start it first, e.g.:" >&2
  echo "  sudo ${BUILD_DIR}/apps/examples/ofh/ru_emulator -c ${REPO_ROOT}/configs/emu.yml" >&2
  echo "Leave it running in another terminal, then re-run this script." >&2
  exit 1
fi
mkdir -p "$OUT_DIR"
echo "== prach A/B benchmark =="
echo "   duration=${DURATION}s  warmup=${WARMUP}s  out=${OUT_DIR}"
echo "   ru_emulator detected (pid $(pgrep -f 'apps/examples/ofh/ru_emulator' | head -1)) — left running, not managed by this script"
echo

GNB_PID=""
STDIN_HOLDER_PIDS=()

cleanup_gnb() {
  [[ -n "$GNB_PID" ]] && kill "$GNB_PID" 2>/dev/null
  sleep 2
  pkill -f "apps/gnb/gnb" 2>/dev/null
  sleep 1
  rm -rf /var/run/dpdk/gnb 2>/dev/null
  rm -f /mnt/huge1G/gnb* /mnt/huge2M/gnb* /mnt/huge/gnb* /dev/hugepages/gnb* 2>/dev/null
  rm -f /dev/shm/gnb* /dev/shm/__rte_gnb* 2>/dev/null
  for hp in "${STDIN_HOLDER_PIDS[@]:-}"; do
    [[ -n "$hp" ]] && kill "$hp" 2>/dev/null
  done
  STDIN_HOLDER_PIDS=()
  rm -f "${OUT_DIR}"/*.fifo 2>/dev/null
  GNB_PID=""
}
trap 'echo; echo "interrupted — cleaning up gnb (ru_emulator left running)"; cleanup_gnb; exit 130' INT TERM

LAST_FIFO=""
make_held_fifo() {
  local fifo="$1"
  rm -f "$fifo"
  mkfifo "$fifo"
  ( exec sleep 100000000 ) <> "$fifo" &
  STDIN_HOLDER_PIDS+=("$!")
  LAST_FIFO="$fifo"
}

set_flag() {
  sed -i -E "s/^([[:space:]]*prach_rx_to_gpu:[[:space:]]*)(true|false)/\\1$1/" "$GNB_YML"
  local got
  got="$(grep -E "^[[:space:]]*prach_rx_to_gpu:" "$GNB_YML" | awk '{print $2}')"
  echo "   prach_rx_to_gpu set to: ${got}"
}

run_one() {
  local label="$1" flag="$2"
  local gnb_log="${OUT_DIR}/gnb_${label}.log"

  echo "-- run: ${label} (prach_rx_to_gpu=${flag}) --"
  set_flag "$flag"
  cleanup_gnb

  make_held_fifo "${OUT_DIR}/gnb_${label}.fifo"
  local gnb_fifo="$LAST_FIFO"
  ( cd "$BUILD_DIR" && OCUDU_PRACH_BENCH=1 exec "$GNB_BIN" -c "$GNB_YML" ) <"$gnb_fifo" >"$gnb_log" 2>&1 &
  GNB_PID=$!
  sleep 6
  if ! kill -0 "$GNB_PID" 2>/dev/null; then
    echo "   ERROR: gnb died on startup — see $gnb_log" >&2
    tail -15 "$gnb_log" >&2
    return 1
  fi

  echo "   warmup ${WARMUP}s ..."
  sleep "$WARMUP"

  echo "   sampling ${DURATION}s ..."
  sleep "$DURATION"

  cleanup_gnb
  echo "   done."
  echo
}

parse_latency() {
  local log="$1" tag="$2"
  grep -E "\[${tag}\] stats:" "$log" 2>/dev/null | awk '
    {
      for (i = 1; i <= NF; ++i) {
        if ($i ~ /^mean=/)  { gsub(/[^0-9.]/,"",$i); m += $i; n++ }
        if ($i ~ /^min=/)   { gsub(/[^0-9.]/,"",$i); if (mn==""||$i<mn) mn=$i }
        if ($i ~ /^max=/)   { gsub(/[^0-9.]/,"",$i); if ($i>mx) mx=$i }
      }
    }
    END {
      if (n>0) printf "%.1f %s %s %d", m/n, (mn==""?"-":mn), (mx==""?"-":mx), n;
      else     printf "- - - 0";
    }'
}

run_one "gpu" "true"  || { echo "GPU run failed"; exit 1; }
run_one "cpu" "false" || { echo "CPU run failed"; exit 1; }

set_flag "true" >/dev/null

read -r GPU_LAT_MEAN GPU_LAT_MIN GPU_LAT_MAX GPU_LAT_N \
  <<<"$(parse_latency "${OUT_DIR}/gnb_gpu.log" "prach_detector_inline")"
read -r CPU_LAT_MEAN CPU_LAT_MIN CPU_LAT_MAX CPU_LAT_N \
  <<<"$(parse_latency "${OUT_DIR}/gnb_cpu.log" "prach_detector_cpu")"

GPU_DET=$(grep -c "prach_pipeline backend=gpu" "${OUT_DIR}/gnb_gpu.log" 2>/dev/null); GPU_DET=${GPU_DET:-0}
CPU_DET=$(grep -c "prach_pipeline backend=cpu" "${OUT_DIR}/gnb_cpu.log" 2>/dev/null); CPU_DET=${CPU_DET:-0}

echo "================================ PRACH A/B RESULT ================================"
printf "%-26s | %-22s | %-22s\n" "metric" "inline GPU (rx_to_gpu)" "default CPU"
printf "%-26s-+-%-22s-+-%-22s\n" "--------------------------" "----------------------" "----------------------"
printf "%-26s | %-22s | %-22s\n" "detector mean latency (us)" "$GPU_LAT_MEAN"  "$CPU_LAT_MEAN"
printf "%-26s | %-22s | %-22s\n" "detector min/max (us)"      "${GPU_LAT_MIN}/${GPU_LAT_MAX}" "${CPU_LAT_MIN}/${CPU_LAT_MAX}"
printf "%-26s | %-22s | %-22s\n" "stats windows parsed"       "$GPU_LAT_N"     "$CPU_LAT_N"
printf "%-26s | %-22s | %-22s\n" "detections logged"          "$GPU_DET"       "$CPU_DET"
echo "==================================================================================="
echo
echo "raw logs in: ${OUT_DIR}"
