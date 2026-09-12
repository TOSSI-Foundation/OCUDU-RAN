#!/usr/bin/env bash
set -euo pipefail

HERE=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)

GNB=${GNB:-$HERE/build_ntn/apps/gnb/gnb}
BASE=${BASE:-$HERE/configs/ntn/leo_rfsim_gnb.yml}
CFG1=${CFG1:-$HERE/configs/ntn/leo_ho_sat1.yml}
CFG2=${CFG2:-$HERE/configs/ntn/leo_ho_sat2.yml}
OAI=${OAI:-$HERE/../OAI_RAN}
UE=${UE:-$OAI/cmake_targets/ran_build/build/nr-uesoftmodem}
UECFG=${UECFG:-$OAI/targets/PROJECTS/GENERIC-NR-5GC/CONF/ue.ntn.leo.rfsim.conf}

EPOCH_LEAD=${EPOCH_LEAD:-12}
HO_AT=${HO_AT:-310}
SAT2_AT=${SAT2_AT:-120}
DUR=${DUR:-820}

TIME_DRIFT=${TIME_DRIFT:--40}

if [[ ${CORE:-sdcore} == oai ]]; then
    IMSI=${IMSI:-001010000000002}; SST=${SST:-1}; SD=${SD:-0xffffff}; DNN=${DNN:-oai}
    UE_KEY=${UE_KEY:-fec86ba6eb707ed08905757b1bb44b8f}; UE_OPC=${UE_OPC:-C42449363BBAD02B66D16BC975D77CC1}
    PING_TARGET=${PING_TARGET:-10.0.0.1}
else
    IMSI=${IMSI:-001010100000001}; SST=${SST:-1}; SD=${SD:-0x010203}; DNN=${DNN:-internet}
    UE_KEY=${UE_KEY:-5122250214c33e723a5dd523fc145fc0}; UE_OPC=${UE_OPC:-981d464c7c52eb6e5036234984ad0bcf}
    PING_TARGET=${PING_TARGET:-8.8.8.8}
fi
BAND=${BAND:-256}
DL_FREQ=${DL_FREQ:-2185000000}
UL_OFFSET=${UL_OFFSET:--190000000}
PRB=${PRB:-52}
SSB=${SSB:-62}

PLMN=${PLMN:-00101}
TAC=${TAC:-1}
PCI1=${PCI1:-1}
PCI2=${PCI2:-2}

LOG1=/tmp/gnb_leo_sat1.log
LOG2=/tmp/gnb_leo_sat2.log
OUT1=/tmp/gnb_leo_sat1_stdout.log
OUT2=/tmp/gnb_leo_sat2_stdout.log
UE_LOG=${UE_LOG:-/tmp/ue_leo_ho.log}
UE_RUNDIR=${UE_RUNDIR:-/tmp/ue_leo_ho_run}
FIFO1=/tmp/gnb_leo_sat1.cmd

for f in "$GNB" "$BASE" "$CFG1" "$CFG2" "$UE" "$UECFG"; do
    [[ -e $f ]] || { echo "missing: $f" >&2; exit 1; }
done

echo "== killing stale processes"
sudo pkill -9 -f '[n]r-uesoftmodem' || true
sudo pkill -9 -f '[a]pps/gnb/gnb'   || true
sleep 2

sudo rm -f "$LOG1" "$LOG2" "$UE_LOG"
rm -f "$OUT1" "$OUT2" "$FIFO1" /tmp/ue_leo_ho_ping.log

cleanup() {
    sudo pkill -9 -f '[n]r-uesoftmodem' 2>/dev/null || true
    sudo pkill -9 -f '[a]pps/gnb/gnb'   2>/dev/null || true
    rm -f "$FIFO1"
}
trap cleanup EXIT

if [[ ${KEEP_EPOCH:-0} == 1 ]]; then
    echo "== KEEP_EPOCH=1, using the epoch already in $BASE"
else
    echo "== stamping epoch ${EPOCH_LEAD}s ahead"
    sed -i "s/epoch_timestamp: '[^']*'/epoch_timestamp: '$(date -u -d "+${EPOCH_LEAD} seconds" '+%Y-%m-%dT%H:%M:%S')'/" "$BASE"
fi
EPOCH_UNIX=$(date -d "$(sed -n "s/^ *epoch_timestamp: '\([^']*\)'.*/\1/p" "$BASE") UTC" +%s)
grep -m1 '^ *epoch_timestamp:' "$BASE"

wait_until() {
    local target=$((EPOCH_UNIX + $1)) now
    now=$(date +%s)
    (( target > now )) && sleep $((target - now)) || true
}

mkdir -p "$UE_RUNDIR"
echo "== starting UE as the rfsimulator server (log: $UE_LOG)"
cd "$UE_RUNDIR"
sudo "$UE" -O "$UECFG" \
    --band "$BAND" -C "$DL_FREQ" --CO "$UL_OFFSET" -r "$PRB" --numerology 0 --ssb "$SSB" \
    --rfsim \
    --rfsimulator.[0].serveraddr server \
    --time-sync-I 0.1 \
    --ntn-initial-time-drift "$TIME_DRIFT" \
    --uicc0.imsi "$IMSI" --uicc0.key "$UE_KEY" --uicc0.opc "$UE_OPC" \
    --uicc0.nssai_sst "$SST" --uicc0.nssai_sd "$SD" \
    --uicc0.pdu_sessions.[0].nssai_sst "$SST" --uicc0.pdu_sessions.[0].nssai_sd "$SD" \
    --uicc0.pdu_sessions.[0].dnn "$DNN" \
    > "$UE_LOG" 2>&1 &
UE_PID=$!

for _ in $(seq 30); do
    ss -ltn 2>/dev/null | grep -q ':4043 ' && break
    sleep 1
done
ss -ltn 2>/dev/null | grep -q ':4043 ' || { echo "UE never listened on 4043:" >&2; sudo tail -30 "$UE_LOG" >&2; exit 1; }

mkfifo "$FIFO1"
exec 9<>"$FIFO1"

echo "== starting gNB for satellite 1 (log: $LOG1)"
sudo "$GNB" -c "$BASE" -c "$CFG1" < "$FIFO1" > "$OUT1" 2>&1 &

echo "== waiting for NG setup on satellite 1"
ng_up() { sudo grep -qaiE "NG Setup Procedure.*(finished successfully|succeeded)" "$LOG1" 2>/dev/null; }
for _ in $(seq 60); do
    ng_up && break
    pgrep -f '[a]pps/gnb/gnb' >/dev/null || { echo "gNB sat1 died:"; cat "$OUT1"; exit 1; }
    sleep 1
done
ng_up || { echo "no NG setup after 60s:"; cat "$OUT1"; sudo tail -20 "$LOG1"; exit 1; }
echo "   NG setup done"

echo "== waiting for the PDU session over satellite 1"
for _ in $(seq 180); do
    sudo grep -qai 'Interface oaitun_ue1 successfully configured' "$UE_LOG" 2>/dev/null && break
    sleep 1
done
ip addr show oaitun_ue1 &>/dev/null || { echo "== NOT attached. UE log tail:" >&2; sudo tail -40 "$UE_LOG" >&2; exit 1; }
echo "== attached via satellite 1 (pci=$PCI1)"
ip -4 addr show oaitun_ue1 | sed -n 's/.*inet \([0-9.]*\).*/   UE IP: \1/p'
ping -I oaitun_ue1 -c 3 -W 5 "${PING_TARGET:-10.0.0.1}" || true

ping -I oaitun_ue1 -D -i 1 "${PING_TARGET:-10.0.0.1}" > /tmp/ue_leo_ho_ping.log 2>&1 &

echo "== T+${SAT2_AT}s: starting gNB for satellite 2 (log: $LOG2)"
wait_until "$SAT2_AT"
sudo "$GNB" -c "$BASE" -c "$CFG2" > "$OUT2" 2>&1 &
for _ in $(seq 60); do
    sudo grep -qaiE "NG Setup Procedure.*(finished successfully|succeeded)" "$LOG2" 2>/dev/null && break
    sleep 1
done
echo "   satellite 2 up"

echo "== T+${HO_AT}s: triggering handover: target at 34 deg and rising"
wait_until "$HO_AT"
RNTI=$(sudo grep -ao 'c-rnti=0x[0-9a-f]*' "$LOG1" | tail -1 | sed 's/.*0x//')
[[ -n $RNTI ]] || { echo "could not read the UE C-RNTI from $LOG1" >&2; exit 1; }
echo "   ho $PCI1 $RNTI $PCI2 $PLMN $TAC"
echo "ho $PCI1 $RNTI $PCI2 $PLMN $TAC" >&9

if [[ ${KILL_SRC:-0} == 1 ]]; then
    sleep 2
    echo "   KILL_SRC: silencing satellite 1 so the target is alone on the air"
    sudo pkill -9 -f 'leo_ho_sat1' || true
fi

echo "== waiting for the handover to complete on satellite 2"
ho_ok() { sudo grep -qai 'rrcReconfigurationComplete' "$LOG2" 2>/dev/null; }
for _ in $(seq 60); do
    ho_ok && break
    sleep 1
done
if ho_ok; then
    echo "== HANDED OVER to satellite 2 (pci=$PCI2)"
    sudo grep -a -m4 -iE 'HandoverRequest|rrcReconfigurationComplete|Path Switch' "$LOG2" | sed 's/^/   /'
else
    echo "== handover did NOT complete." >&2
    echo "-- source, handover signalling:" >&2
    sudo grep -aiE 'Handover|XNAP.*(Request|Acknowledge)|rrcReconfiguration' "$LOG1" | tail -6 >&2
    echo "-- target, handover signalling:" >&2
    sudo grep -aiE 'Handover|prach|UE created|rrcReconfigurationComplete|timeout' "$LOG2" | tail -6 >&2
    echo "-- UE, what it made of the command (expect 're-sync detection for target Nid_cell 2'):" >&2
    sudo grep -aiE 're-sync detection|pbch not decoded|In synch|RAR|reestablish' "$UE_LOG" | tail -6 >&2
fi

echo
echo "logs: $LOG1  $LOG2  $UE_LOG   ping: /tmp/ue_leo_ho_ping.log"
echo "tracker: $HERE/scripts/leo_tracker.py $LOG1   (and $LOG2 on --port 8501)"
echo "running to T+${DUR}s; Ctrl-C to stop."
wait_until "$DUR"
