#!/usr/bin/env bash
set -euo pipefail

HERE=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)

GNB=${GNB:-$HERE/build_ntn/apps/gnb/gnb}
CFG=${CFG:-$HERE/configs/ntn/leo_rfsim_gnb.yml}
OAI=${OAI:-$HERE/../OAI_RAN}
UE=${UE:-$OAI/cmake_targets/ran_build/build/nr-uesoftmodem}
UECFG=${UECFG:-$OAI/targets/PROJECTS/GENERIC-NR-5GC/CONF/ue.ntn.leo.rfsim.conf}

EPOCH_LEAD=${EPOCH_LEAD:-12}

INITIAL_FO=${INITIAL_FO:-50411}
TIME_DRIFT=${TIME_DRIFT:--40}

BAND=${BAND:-256}
DL_FREQ=${DL_FREQ:-2185000000}
UL_OFFSET=${UL_OFFSET:--190000000}
PRB=${PRB:-52}
SSB=${SSB:-62}

GNB_LOG=${GNB_LOG:-/tmp/gnb_leo.log}
GNB_STDOUT=${GNB_STDOUT:-/tmp/gnb_leo_stdout.log}
UE_LOG=${UE_LOG:-/tmp/ue_leo.log}
UE_RUNDIR=${UE_RUNDIR:-/tmp/ue_leo_run}

for f in "$GNB" "$CFG" "$UE" "$UECFG"; do
    [[ -e $f ]] || { echo "missing: $f" >&2; exit 1; }
done

echo "== killing stale processes"
sudo pkill -9 -f nr-uesoftmodem || true
sudo pkill -9 -f 'apps/gnb/gnb' || true
sleep 2

sudo rm -f "$GNB_LOG"
rm -f "$GNB_STDOUT"

if [[ ${KEEP_EPOCH:-0} == 1 ]]; then
    echo "== KEEP_EPOCH=1, using the epoch already in $CFG"
else
    echo "== stamping epoch ${EPOCH_LEAD}s ahead, for the moment the UE connects"
    sed -i "s/epoch_timestamp: '[^']*'/epoch_timestamp: '$(date -u -d "+${EPOCH_LEAD} seconds" '+%Y-%m-%dT%H:%M:%S')'/" "$CFG"
fi
grep -m1 '^ *epoch_timestamp:' "$CFG"

echo "== starting gNB  (log: $GNB_LOG)"
sudo "$GNB" -c "$CFG" > "$GNB_STDOUT" 2>&1 &
trap 'sudo pkill -9 -f apps/gnb/gnb 2>/dev/null || true' EXIT

echo "== waiting for NG setup"
ng_up() { sudo grep -qaiE "NG Setup Procedure.*(finished successfully|succeeded)" "$GNB_LOG" 2>/dev/null; }
for _ in $(seq 60); do
    ng_up && break
    pgrep -f "apps/gnb/gnb" >/dev/null || { echo "gNB died:"; cat "$GNB_STDOUT"; exit 1; }
    sleep 1
done
ng_up || { echo "no NG setup after 60s:"; cat "$GNB_STDOUT"; sudo tail -20 "$GNB_LOG" 2>/dev/null; exit 1; }
echo "   NG setup done"

for _ in $(seq 30); do
    ss -ltn 2>/dev/null | grep -q ':4043 ' && break
    sleep 1
done

mkdir -p "$UE_RUNDIR"
echo "== starting UE   (log: $UE_LOG)"
cd "$UE_RUNDIR"
sudo "$UE" -O "$UECFG" \
    --band "$BAND" -C "$DL_FREQ" --CO "$UL_OFFSET" -r "$PRB" --numerology 0 --ssb "$SSB" \
    --rfsim \
    --rfsimulator.[0].serveraddr 127.0.0.1 \
    --time-sync-I 0.1 \
    --ntn-initial-time-drift "$TIME_DRIFT" \
    > "$UE_LOG" 2>&1 &
UE_PID=$!
trap 'sudo pkill -9 -f nr-uesoftmodem 2>/dev/null || true; sudo pkill -9 -f apps/gnb/gnb 2>/dev/null || true' EXIT

echo "== waiting for the PDU session"
for _ in $(seq 180); do
    sudo grep -qai 'Interface oaitun_ue1 successfully configured' "$UE_LOG" 2>/dev/null && break
    sleep 1
done

if ip addr show oaitun_ue1 &>/dev/null; then
    echo "== attached"
    ip -4 addr show oaitun_ue1 | sed -n 's/.*inet \([0-9.]*\).*/   UE IP: \1/p'
    echo "== ping over the LEO link (expect ~30-40 ms RTT, falling as the satellite climbs)"
    ping -I oaitun_ue1 -c 4 -W 5 "${PING_TARGET:-10.0.0.1}" || true
else
    echo "== NOT attached. UE log tail:" >&2
    sudo tail -40 "$UE_LOG" >&2
    exit 1
fi

echo
echo "gNB log: $GNB_LOG   UE log: $UE_LOG"
echo "Ctrl-C to stop both."
wait $UE_PID
