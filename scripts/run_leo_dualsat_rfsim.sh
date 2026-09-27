#!/usr/bin/env bash
# TWO LEO satellites serving one UE AT THE SAME TIME, positioned by the OAI LMF over the rfsimulator.
set -euo pipefail

HERE=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)

GNB=${GNB:-$HERE/build_ntn/apps/gnb/gnb}
BASE=${BASE:-$HERE/configs/ntn/leo_rfsim_gnb.yml}
CFG1=${CFG1:-$HERE/configs/ntn/leo_dualsat_sat1.yml}
CFG2=${CFG2:-$HERE/configs/ntn/leo_dualsat_sat2.yml}
OAI=${OAI:-$(dirname "$HERE")/OAI_RAN}
UE=${UE:-$OAI/cmake_targets/ran_build/build/nr-uesoftmodem}
UECFG=${UECFG:-$OAI/targets/PROJECTS/GENERIC-NR-5GC/CONF/ue.ntn.leo.rfsim.conf}
CORE=${CORE:-$HOME/oai-cn5g}
LMF_NTN_OAM=${LMF_NTN_OAM:-$CORE/conf/ntn/ntn_satellites.json}

EPOCH_LEAD=${EPOCH_LEAD:-12}
# When satellite 2's gNB joins, counted from the epoch. Late enough that the UE has attached to satellite 1
# without a second cell on the air during acquisition, early enough to leave most of the pass with both.
SAT2_AT=${SAT2_AT:-90}
DUR=${DUR:-620}
TIME_DRIFT=${TIME_DRIFT:--40}

LOG1=${GNB_LOG:-/tmp/gnb_dualsat_sat1.log}
LOG2=${GNB_LOG2:-/tmp/gnb_dualsat_sat2.log}
OUT1=/tmp/gnb_dualsat_sat1_stdout.log
OUT2=/tmp/gnb_dualsat_sat2_stdout.log
UE_LOG=${UE_LOG:-/tmp/ue_dualsat.log}
UE_RUNDIR=${UE_RUNDIR:-/tmp/ue_dualsat_run}
PING_LOG=/tmp/ue_dualsat_ping.log

BAND=${BAND:-256}; DL_FREQ=${DL_FREQ:-2185000000}; UL_OFFSET=${UL_OFFSET:--190000000}
PRB=${PRB:-52}; SSB=${SSB:-62}
# The second address the two gNBs need: one host cannot bind GTP-U 2152 twice on 192.168.70.129.
CORE_IF=${CORE_IF:-oai-cn5g}
GNB2_ADDR=${GNB2_ADDR:-192.168.70.141/26}

for f in "$GNB" "$BASE" "$CFG1" "$CFG2" "$UE" "$UECFG"; do
    [[ -e $f ]] || { echo "missing: $f" >&2; exit 1; }
done

# The terminal handling of scripts/run_leo_rfsim.sh: sudo under a use_pty sudoers leaves the tty raw if a
# background child is killed mid-write, and a bare pkill pattern matches this script's own command line.
TTY_STATE=$(stty -g 2>/dev/null || true)
kill_stack() {
    sudo pkill -9 -f '[n]r-uesoftmodem' 2>/dev/null || true
    sudo pkill -9 -f '[a]pps/gnb/gnb'   2>/dev/null || true
    for _ in $(seq 20); do pgrep -f '[n]r-uesoftmodem|[a]pps/gnb/gnb' >/dev/null || break; sleep 0.25; done
}
cleanup() {
    trap - EXIT INT TERM
    kill_stack
    trap '' TTOU
    [[ -n $TTY_STATE ]] && stty "$TTY_STATE" 2>/dev/null
    return 0
}
trap cleanup EXIT INT TERM

echo "== killing stale processes"
kill_stack
sudo rm -f "$LOG1" "$LOG2" "$UE_LOG"
rm -f "$OUT1" "$OUT2" "$PING_LOG"

# The LMF waits for lmf.num_gnb gNBs before it positions, and there are two now.
if ! grep -qE '^ *num_gnb: *2' "$CORE/conf/config.yaml"; then
    echo "== setting lmf.num_gnb = 2 in $CORE/conf/config.yaml (put it back to 1 for the single-satellite runs)"
    sudo sed -i 's/^\( *num_gnb:\) *[0-9]*/\1 2/' "$CORE/conf/config.yaml"
fi
ip -4 addr show dev "$CORE_IF" 2>/dev/null | grep -q "${GNB2_ADDR%/*}" || {
    echo "== adding $GNB2_ADDR to $CORE_IF for the second gNB"
    sudo ip addr add "$GNB2_ADDR" dev "$CORE_IF"
}
echo "== restarting oai-lmf so it starts from an empty measurement history"
(cd "$CORE" && docker compose restart oai-lmf >/dev/null)

if [[ ${KEEP_EPOCH:-0} == 1 ]]; then
    echo "== KEEP_EPOCH=1, using the epoch already in $BASE"
else
    echo "== stamping epoch ${EPOCH_LEAD}s ahead"
    sudo sed -i "s/epoch_timestamp: '[^']*'/epoch_timestamp: '$(date -u -d "+${EPOCH_LEAD} seconds" '+%Y-%m-%dT%H:%M:%S')'/" "$BASE"
fi
EPOCH_UNIX=$(date -d "$(sed -n "s/^ *epoch_timestamp: '\([^']*\)'.*/\1/p" "$BASE") UTC" +%s)
grep -m1 '^ *epoch_timestamp:' "$BASE"

# The LMF's OAM input (TS 38.305 5.4): both satellites, each carrying its own gNB's TRP, neither with a window.
GNB2_ID=$(sed -n 's/^gnb_id: *\([0-9]*\).*/\1/p' "$CFG2")
"$HERE/scripts/lmf_ntn_oam.py" "$BASE" "$LMF_NTN_OAM" --add "$CFG2:$GNB2_ID"

wait_until() { local target=$((EPOCH_UNIX + $1)) now; now=$(date +%s); (( target > now )) && sleep $((target - now)) || true; }

# THE gNB STARTS FIRST, even though the UE is the rfsimulator server.
echo "== starting the gNB for satellite 1 (log: $LOG1)"
setsid sudo "$GNB" -c "$BASE" -c "$CFG1" < /dev/null > "$OUT1" 2>&1 &
disown
ng_up() { sudo grep -qaiE "NG Setup Procedure.*(finished successfully|succeeded)" "$1" 2>/dev/null; }
for _ in $(seq 60); do
    ng_up "$LOG1" && break
    pgrep -f '[a]pps/gnb/gnb' >/dev/null || { echo "the satellite 1 gNB died:"; cat "$OUT1"; exit 1; }
    sleep 1
done
ng_up "$LOG1" || { echo "no NG setup after 60s:"; cat "$OUT1"; sudo tail -20 "$LOG1"; exit 1; }
echo "   NG setup done"

mkdir -p "$UE_RUNDIR"
echo "== starting the UE as the rfsimulator server (log: $UE_LOG)"
cd "$UE_RUNDIR"
setsid sudo "$UE" -O "$UECFG" \
    --band "$BAND" -C "$DL_FREQ" --CO "$UL_OFFSET" -r "$PRB" --numerology 0 --ssb "$SSB" \
    --rfsim --rfsimulator.[0].serveraddr server \
    --time-sync-I 0.1 --ntn-initial-time-drift "$TIME_DRIFT" \
    < /dev/null > "$UE_LOG" 2>&1 &
UE_PID=$!
disown

for _ in $(seq 30); do ss -ltn 2>/dev/null | grep -q ':4043 ' && break; sleep 1; done
ss -ltn 2>/dev/null | grep -q ':4043 ' || { echo "the UE never listened on 4043:" >&2; sudo tail -30 "$UE_LOG" >&2; exit 1; }

# Long, on purpose. NAS registration runs over a ~19 ms round trip at the rising horizon and the base config
# already stretches request_pdu_session_timeout to 120 s for it; measured here, random access alone completed
# at about T+190. A 180 s wait reports "NOT attached" on a UE that is in fact most of the way in.
echo "== waiting for the PDU session over satellite 1 (up to ${ATTACH_WAIT:-360}s)"
for _ in $(seq "${ATTACH_WAIT:-360}"); do
    sudo grep -qai 'Interface oaitun_ue1 successfully configured' "$UE_LOG" 2>/dev/null && break
    sleep 1
done
ip addr show oaitun_ue1 &>/dev/null || { echo "== NOT attached. UE log tail:" >&2; sudo tail -40 "$UE_LOG" >&2; exit 1; }
echo "== attached via satellite 1"
ip -4 addr show oaitun_ue1 | sed -n 's/.*inet \([0-9.]*\).*/   UE IP: \1/p'
ping -I oaitun_ue1 -D -i 1 10.0.0.1 > "$PING_LOG" 2>&1 &

echo "== T+${SAT2_AT}s: starting the gNB for satellite 2 (log: $LOG2)"
wait_until "$SAT2_AT"
setsid sudo "$GNB" -c "$BASE" -c "$CFG2" < /dev/null > "$OUT2" 2>&1 &
disown
for _ in $(seq 60); do ng_up "$LOG2" && break; sleep 1; done
ng_up "$LOG2" || { echo "satellite 2 never completed NG setup:"; cat "$OUT2"; sudo tail -20 "$LOG2"; exit 1; }
echo "   satellite 2 up, both TRPs now serving"

echo
echo "gNB logs: $LOG1  $LOG2"
echo "UE log:   $UE_LOG"
echo "running to T+${DUR}s; Ctrl-C to stop."
wait_until "$DUR"

echo "== both satellites, as the LMF saw them"
docker logs oai-lmf 2>&1 | grep -c "neighbour dl-PRS-ID" | sed 's/^/   neighbour ranges: /' || true
docker logs oai-lmf 2>&1 | grep "NTN Multi-RTT fix" | tail -3 | sed 's/^/   /' || true
