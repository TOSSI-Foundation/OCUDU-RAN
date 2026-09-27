#!/usr/bin/env bash
# Satellite switch with re-sync over the rfsimulator: ONE OCUDU gNB flying two satellites, one OAI nr-
# uesoftmodem.
set -euo pipefail

HERE=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)

GNB=${GNB:-$HERE/build_ntn/apps/gnb/gnb}
BASE=${BASE:-$HERE/configs/ntn/leo_rfsim_gnb.yml}
CFG=${CFG:-$HERE/configs/ntn/leo_satswitch.yml}
UE_CONF=ue.ntn.leo.rfsim.conf
# The OAI UE tree: the sibling of this repo that actually carries the config this script needs, so a stale
# clone under another name is skipped rather than picked. Set OAI to choose one yourself.
OAI=${OAI:-}
if [[ -z $OAI ]]; then
    for d in OAI-RAN-rfsim OAI_RAN OAI-RAN openairinterface5g; do
        cand=$(dirname "$HERE")/$d
        [[ -f $cand/targets/PROJECTS/GENERIC-NR-5GC/CONF/$UE_CONF ]] && OAI=$cand && break
    done
fi
OAI=${OAI:-$(dirname "$HERE")/OAI-RAN-rfsim}
UE=${UE:-$OAI/cmake_targets/ran_build/build/nr-uesoftmodem}
UECFG=${UECFG:-$OAI/targets/PROJECTS/GENERIC-NR-5GC/CONF/ue.ntn.leo.rfsim.conf}

EPOCH_LEAD=${EPOCH_LEAD:-12}
# Seconds after the epoch at which satellite 1 stops serving and satellite 2 takes over. 252 s is the crossover,
# where both satellites are at 28.8 degrees and therefore at the same range - so the link delay is continuous
# across the switch and only its drift reverses. See the pass table in configs/ntn/leo_satswitch.yml.
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

# The terminal as it is now, to put back on exit: sudoers has use_pty here, and a sudo'd process in the
# background leaves the terminal without output newline translation (see scripts/run_leo_rfsim.sh).
TTY_STATE=$(stty -g 2>/dev/null || true)
cleanup() {
    trap - EXIT INT TERM
    sudo pkill -9 -f '[n]r-uesoftmodem' 2>/dev/null || true
    sudo pkill -9 -f '[a]pps/gnb/gnb'   2>/dev/null || true
    sudo pkill -9 -f '[p]ing -I oaitun' 2>/dev/null || true
    trap '' TTOU   # under `timeout` this is a background process group: setting the terminal would stop it
    [[ -n $TTY_STATE ]] && stty "$TTY_STATE" 2>/dev/null
    return 0
}
trap cleanup EXIT INT TERM

# Three timestamps have to agree: the epoch both satellites are propagated from, and the two halves of the switch
# instant. They are stamped together here so the switch always lands on the crossover regardless of when the run
# starts. The epoch lives in the base config and the switch times in the overlay.
echo "== stamping epoch ${EPOCH_LEAD}s ahead, switch at T+${SWITCH_AT}s"
EPOCH_ISO=$(date -u -d "+${EPOCH_LEAD} seconds" '+%Y-%m-%dT%H:%M:%S')
SWITCH_ISO=$(date -u -d "+$((EPOCH_LEAD + SWITCH_AT)) seconds" '+%Y-%m-%dT%H:%M:%S')
EPOCH_UNIX=$(date -d "$EPOCH_ISO UTC" +%s)

sed -i "s/epoch_timestamp: '[^']*'/epoch_timestamp: '$EPOCH_ISO'/" "$BASE"
# Both satellites share one epoch, so the overlay's sat-switch epoch is stamped to the same value.
sed -i "s/epoch_timestamp: '[^']*'/epoch_timestamp: '$EPOCH_ISO'/" "$CFG"
sed -i "s/t_service: '[^']*'/t_service: '$SWITCH_ISO'/" "$CFG"
sed -i "s/t_service_start: '[^']*'/t_service_start: '$SWITCH_ISO'/" "$CFG"
# The LMF's OAM satellite information (TS 38.305 5.4) for THIS run: both satellites and the instant the cell
# changes hands, so the LMF knows which one carried the TRP for each measurement. LMF_NTN_OAM="" skips it.
LMF_NTN_OAM=${LMF_NTN_OAM-$HOME/oai-cn5g/conf/ntn/ntn_satellites.json}
if [[ -n $LMF_NTN_OAM ]]; then
    python3 "$HERE/scripts/lmf_ntn_oam.py" "$BASE" "$LMF_NTN_OAM" --switch-cfg "$CFG"
fi
echo "   epoch  $EPOCH_ISO"
echo "   switch $SWITCH_ISO"

wait_until() {
    local target=$((EPOCH_UNIX + $1)) now
    now=$(date +%s)
    (( target > now )) && sleep $((target - now)) || true
}

echo "== starting gNB (log: $GNB_LOG)"
# setsid + stdin from /dev/null: neither process may own the terminal (sudoers has use_pty).
setsid sudo "$GNB" -c "$BASE" -c "$CFG" ${EXTRA_CFG:+-c "$EXTRA_CFG"} < /dev/null > "$GNB_STDOUT" 2>&1 &
disown

echo "== waiting for NG setup"
ng_up() { sudo grep -qaiE "NG Setup Procedure.*(finished successfully|succeeded)" "$GNB_LOG" 2>/dev/null; }
for _ in $(seq 60); do
    ng_up && break
    pgrep -f '[a]pps/gnb/gnb' >/dev/null || { echo "gNB died:"; cat "$GNB_STDOUT"; exit 1; }
    sleep 1
done
ng_up || { echo "no NG setup after 60s:"; cat "$GNB_STDOUT"; sudo tail -20 "$GNB_LOG"; exit 1; }
echo "   NG setup done"

# Confirm the promotion was actually scheduled. If this line is absent the run is pointless: the switch is only
# being advertised in SIB19 and the cell will fly satellite 1 straight past its horizon.
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
setsid sudo "$UE" -O "$UECFG" \
    --band "$BAND" -C "$DL_FREQ" --CO "$UL_OFFSET" -r "$PRB" --numerology 0 --ssb "$SSB" \
    --rfsim \
    --rfsimulator.[0].serveraddr 127.0.0.1 \
    --time-sync-I 0.1 \
    --ntn-initial-time-drift "$TIME_DRIFT" \
    ${UE_EXTRA_ARGS:-} \
    < /dev/null > "$UE_LOG" 2>&1 &
disown

echo "== waiting for the PDU session"
for _ in $(seq 180); do
    sudo grep -qai 'Interface oaitun_ue1 successfully configured' "$UE_LOG" 2>/dev/null && break
    sleep 1
done
ip addr show oaitun_ue1 &>/dev/null || { echo "== NOT attached. UE log tail:" >&2; sudo tail -40 "$UE_LOG" >&2; exit 1; }
echo "== attached via satellite 1"
ip -4 addr show oaitun_ue1 | sed -n 's/.*inet \([0-9.]*\).*/   UE IP: \1/p'

# The whole result is in this ping. A satellite switch the UE survives shows up as an unbroken sequence across
# t_service; one it does not shows up as the sequence stopping there.
ping -I oaitun_ue1 -D -i 1 "${PING_TARGET:-10.0.0.1}" > "$PING_LOG" 2>&1 &

echo "== running to the switch at T+${SWITCH_AT}s"
wait_until $((SWITCH_AT + 25))

echo
echo "== switch window"
sudo grep -aiE 'Sat-switch|satellite .* -> |horizon' "$GNB_LOG" | tail -5 | sed 's/^/   /' || true
echo "-- emulated channel either side of the switch (delay should be continuous, drift should reverse):"
sudo grep -a 'Emulated NTN channel' "$GNB_LOG" | awk 'NR%40==0' | tail -8 | sed 's/.*\(rx_delay=[0-9.]*us drift=[-0-9.]*\).*/   \1/'

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
