#!/usr/bin/env bash
set -euo pipefail

HERE=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)

GNB=${GNB:-$HERE/build_ntn/apps/gnb/gnb}
BASE=${BASE:-$HERE/configs/ntn/leo_rfsim_gnb.yml}
CFG=${CFG:-$HERE/configs/ntn/leo_satswitch.yml}
OAI=${OAI:-$HERE/../OAI_RAN}
UE=${UE:-$OAI/cmake_targets/ran_build/build/nr-uesoftmodem}
UECFG=${UECFG:-$OAI/targets/PROJECTS/GENERIC-NR-5GC/CONF/ue.ntn.leo.rfsim.conf}

EPOCH_LEAD=${EPOCH_LEAD:-12}
SWITCH_AT=${SWITCH_AT:-252}
DUR=${DUR:-700}

TIME_DRIFT=${TIME_DRIFT:--40}
BAND=${BAND:-256}
DL_FREQ=${DL_FREQ:-2185000000}
UL_OFFSET=${UL_OFFSET:--190000000}
PRB=${PRB:-52}
SSB=${SSB:-62}

GNB_LOG=${GNB_LOG:-/tmp/gnb_leo_satswitch.log}
GNB_STDOUT=${GNB_STDOUT:-/tmp/gnb_leo_satswitch_stdout.log}
UE_LOG=${UE_LOG:-/tmp/ue_leo_satswitch.log}
UE_RUNDIR=${UE_RUNDIR:-/tmp/ue_leo_satswitch_run}
PING_LOG=${PING_LOG:-/tmp/ue_leo_satswitch_ping.log}

for f in "$GNB" "$BASE" "$CFG" "$UE" "$UECFG"; do
    [[ -e $f ]] || { echo "missing: $f" >&2; exit 1; }
done

echo "== killing stale processes"
sudo pkill -9 -f '[n]r-uesoftmodem' || true
sudo pkill -9 -f '[a]pps/gnb/gnb'   || true
sudo pkill -9 -f '[p]ing -I oaitun' || true
sleep 2

sudo rm -f "$GNB_LOG"
rm -f "$GNB_STDOUT" "$PING_LOG"

cleanup() {
    sudo pkill -9 -f '[n]r-uesoftmodem' 2>/dev/null || true
    sudo pkill -9 -f '[a]pps/gnb/gnb'   2>/dev/null || true
    sudo pkill -9 -f '[p]ing -I oaitun' 2>/dev/null || true
}
trap cleanup EXIT

echo "== stamping epoch ${EPOCH_LEAD}s ahead, switch at T+${SWITCH_AT}s"
EPOCH_ISO=$(date -u -d "+${EPOCH_LEAD} seconds" '+%Y-%m-%dT%H:%M:%S')
SWITCH_ISO=$(date -u -d "+$((EPOCH_LEAD + SWITCH_AT)) seconds" '+%Y-%m-%dT%H:%M:%S')
EPOCH_UNIX=$(date -d "$EPOCH_ISO UTC" +%s)

sed -i "s/epoch_timestamp: '[^']*'/epoch_timestamp: '$EPOCH_ISO'/" "$BASE"
sed -i "s/epoch_timestamp: '[^']*'/epoch_timestamp: '$EPOCH_ISO'/" "$CFG"
sed -i "s/t_service: '[^']*'/t_service: '$SWITCH_ISO'/" "$CFG"
sed -i "s/t_service_start: '[^']*'/t_service_start: '$SWITCH_ISO'/" "$CFG"
echo "   epoch  $EPOCH_ISO"
echo "   switch $SWITCH_ISO"

wait_until() {
    local target=$((EPOCH_UNIX + $1)) now
    now=$(date +%s)
    (( target > now )) && sleep $((target - now)) || true
}

echo "== starting gNB (log: $GNB_LOG)"
sudo "$GNB" -c "$BASE" -c "$CFG" > "$GNB_STDOUT" 2>&1 &

echo "== waiting for NG setup"
ng_up() { sudo grep -qaiE "NG Setup Procedure.*(finished successfully|succeeded)" "$GNB_LOG" 2>/dev/null; }
for _ in $(seq 60); do
    ng_up && break
    pgrep -f '[a]pps/gnb/gnb' >/dev/null || { echo "gNB died:"; cat "$GNB_STDOUT"; exit 1; }
    sleep 1
done
ng_up || { echo "no NG setup after 60s:"; cat "$GNB_STDOUT"; sudo tail -20 "$GNB_LOG"; exit 1; }
echo "   NG setup done"

if sudo grep -qa 'Sat-switch promotion scheduled' "$GNB_LOG"; then
    sudo grep -a -m1 'Sat-switch promotion scheduled' "$GNB_LOG" | sed 's/^/   /'
else
    echo "!! no sat-switch promotion scheduled - check t_service and promote_to_serving" >&2
fi

for _ in $(seq 30); do
    ss -ltn 2>/dev/null | grep -q ':4043 ' && break
    sleep 1
done

mkdir -p "$UE_RUNDIR"
echo "== starting UE (log: $UE_LOG)"
cd "$UE_RUNDIR"
sudo "$UE" -O "$UECFG" \
    --band "$BAND" -C "$DL_FREQ" --CO "$UL_OFFSET" -r "$PRB" --numerology 0 --ssb "$SSB" \
    --rfsim \
    --rfsimulator.[0].serveraddr 127.0.0.1 \
    --time-sync-I 0.1 \
    --ntn-initial-time-drift "$TIME_DRIFT" \
    > "$UE_LOG" 2>&1 &

echo "== waiting for the PDU session"
for _ in $(seq 180); do
    sudo grep -qai 'Interface oaitun_ue1 successfully configured' "$UE_LOG" 2>/dev/null && break
    sleep 1
done
ip addr show oaitun_ue1 &>/dev/null || { echo "== NOT attached. UE log tail:" >&2; sudo tail -40 "$UE_LOG" >&2; exit 1; }
echo "== attached via satellite 1"
ip -4 addr show oaitun_ue1 | sed -n 's/.*inet \([0-9.]*\).*/   UE IP: \1/p'

ping -I oaitun_ue1 -D -i 1 "${PING_TARGET:-10.0.0.1}" > "$PING_LOG" 2>&1 &

echo "== running to the switch at T+${SWITCH_AT}s"
wait_until $((SWITCH_AT + 25))

echo
echo "== switch window"
sudo grep -aiE 'Sat-switch|satellite .* -> |horizon' "$GNB_LOG" | tail -5 | sed 's/^/   /' || true
echo "-- emulated channel either side of the switch (delay should be continuous, drift should reverse):"
sudo grep -a 'Emulated NTN channel' "$GNB_LOG" | awk 'NR%40==0' | tail -8 | sed 's/.*\(rx_delay=[0-9]*us drift=[-0-9.]*\).*/   \1/'

echo
echo "-- ping across the switch:"
tail -6 "$PING_LOG" | sed 's/^/   /'
if ip addr show oaitun_ue1 &>/dev/null && tail -3 "$PING_LOG" | grep -q 'bytes from'; then
    echo "== SESSION SURVIVED the satellite switch"
else
    echo "== session did NOT survive the switch" >&2
    sudo grep -aiE 'synch Failed|T430|out of sync|RRC.*[Rr]elease|reestablish' "$UE_LOG" | tail -6 >&2
fi

echo
echo "gNB log: $GNB_LOG   UE log: $UE_LOG   ping: $PING_LOG"
echo "running to T+${DUR}s; Ctrl-C to stop."
wait_until "$DUR"

echo "== final ping statistics"
sudo pkill -INT -f '[p]ing -I oaitun' 2>/dev/null || true
sleep 1
tail -5 "$PING_LOG" | sed 's/^/   /'
