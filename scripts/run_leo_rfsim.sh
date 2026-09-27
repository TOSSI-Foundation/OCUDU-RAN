#!/usr/bin/env bash
# Transparent LEO NTN bring-up: OCUDU gNB + OAI nr-uesoftmodem over the rfsimulator.
set -euo pipefail

HERE=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)

GNB=${GNB:-$HERE/build_ntn/apps/gnb/gnb}
# Optional second -c overlay, e.g.
CFG=${CFG:-$HERE/configs/ntn/leo_rfsim_gnb.yml}
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

# Radio parameters, matching cell_cfg in $CFG: n256, 2185 MHz DL, UL 190 MHz below, 52 PRB, 15 kHz SCS.
EPOCH_LEAD=${EPOCH_LEAD:-12}

INITIAL_FO=${INITIAL_FO:-50411}
TIME_DRIFT=${TIME_DRIFT:--40}

BAND=${BAND:-256}
DL_FREQ=${DL_FREQ:-2185000000}
UL_OFFSET=${UL_OFFSET:--190000000}
PRB=${PRB:-52}
SSB=${SSB:-62}

# The gNB writes its own log to log.filename in $CFG. The script's stdout redirect must go somewhere else:
# that file is created by root, and a non-root shell reopening it next run gets EACCES, which left the gNB
# unstarted while the NG-setup wait below matched a stale success line from the previous run.
GNB_LOG=${GNB_LOG:-/tmp/gnb_leo.log}
GNB_STDOUT=${GNB_STDOUT:-/tmp/gnb_leo_stdout.log}
UE_LOG=${UE_LOG:-/tmp/ue_leo.log}
UE_RUNDIR=${UE_RUNDIR:-/tmp/ue_leo_run}

for f in "$GNB" "$CFG" "$UE" "$UECFG"; do
    [[ -e $f ]] || { echo "missing: $f" >&2; exit 1; }
done

# The terminal as it is now, to put back on exit: a sudo'd process in the background can leave it without
# output newline translation (every line then starts where the last one ended).
TTY_STATE=$(stty -g 2>/dev/null || true)
# [x]yz patterns: a plain "pkill -f nr-uesoftmodem" matches its own command line (and sudo's), kills itself and
# prints "Killed".
kill_stack() {
    sudo pkill -9 -f '[n]r-uesoftmodem' 2>/dev/null || true
    sudo pkill -9 -f '[a]pps/gnb/gnb' 2>/dev/null || true
    for _ in $(seq 20); do   # gone, not just signalled: a rerun right after must find the rfsim port free
        pgrep -f '[n]r-uesoftmodem|[a]pps/gnb/gnb' >/dev/null || break
        sleep 0.25
    done
}
cleanup() {
    trap - EXIT INT TERM
    kill_stack
    # Under `timeout` this script is a background process group, and setting the terminal from there stops it
    # with SIGTTOU unless that is ignored.
    trap '' TTOU
    [[ -n $TTY_STATE ]] && stty "$TTY_STATE" 2>/dev/null
    return 0
}
trap cleanup EXIT INT TERM

# A leftover UE or gNB holds the rfsimulator port and the next run fails to bind.
echo "== killing stale processes"
kill_stack
sleep 2

# Drop the previous run's log: it is root-owned, which both blocks the gNB from recreating it and lets the
# NG-setup wait match a success line from an earlier run.
sudo rm -f "$GNB_LOG"
rm -f "$GNB_STDOUT"

# The epoch must be stamped BEFORE the gNB starts, since the gNB parses it once at startup - but it has to
# name the moment the UE CONNECTS, not now.
if [[ ${KEEP_EPOCH:-0} == 1 ]]; then
    echo "== KEEP_EPOCH=1, using the epoch already in $CFG"
else
    echo "== stamping epoch ${EPOCH_LEAD}s ahead, for the moment the UE connects"
    sed -i "s/epoch_timestamp: '[^']*'/epoch_timestamp: '$(date -u -d "+${EPOCH_LEAD} seconds" '+%Y-%m-%dT%H:%M:%S')'/" "$CFG"
fi
grep -m1 '^ *epoch_timestamp:' "$CFG"
# The LMF's OAM satellite information (TS 38.305 5.4) must carry this run's epoch: rewrite it now. LMF_NTN_OAM=""
# skips it (no positioning in the run).
LMF_NTN_OAM=${LMF_NTN_OAM-$HOME/oai-cn5g/conf/ntn/ntn_satellites.json}
if [[ -n $LMF_NTN_OAM ]]; then
    python3 "$HERE/scripts/lmf_ntn_oam.py" "$CFG" "$LMF_NTN_OAM"
fi

echo "== starting gNB  (log: $GNB_LOG)"
# setsid + stdin from /dev/null: neither may touch the terminal. sudoers here has use_pty, and a sudo in the
# background with a controlling terminal puts that terminal into raw mode and leaves it there.
setsid sudo "$GNB" -c "$CFG" ${EXTRA_CFG:+-c "$EXTRA_CFG"} < /dev/null > "$GNB_STDOUT" 2>&1 &
disown   # no "Killed ..." job report when cleanup kills it

# N2 is SCTP, so a TCP probe against 38412 proves nothing. Wait for the gNB to say the procedure succeeded.
echo "== waiting for NG setup"
ng_up() { sudo grep -qaiE "NG Setup Procedure.*(finished successfully|succeeded)" "$GNB_LOG" 2>/dev/null; }
for _ in $(seq 60); do
    ng_up && break
    pgrep -f "[a]pps/gnb/gnb" >/dev/null || { echo "gNB died:"; cat "$GNB_STDOUT"; exit 1; }
    sleep 1
done
ng_up || { echo "no NG setup after 60s:"; cat "$GNB_STDOUT"; sudo tail -20 "$GNB_LOG" 2>/dev/null; exit 1; }
echo "   NG setup done"

# The rfsimulator only listens once the RU is up; connecting before that just burns UE retries.
for _ in $(seq 30); do
    ss -ltn 2>/dev/null | grep -q ':4043 ' && break
    sleep 1
done

# sudo: the UE needs CAP_NET_ADMIN to create oaitun_ue1. Without it the attach completes but the data plane
# never comes up. It also writes files into its working directory, so run it somewhere writable.
mkdir -p "$UE_RUNDIR"
echo "== starting UE   (log: $UE_LOG)"
cd "$UE_RUNDIR"
setsid sudo "$UE" -O "$UECFG" \
    --band "$BAND" -C "$DL_FREQ" --CO "$UL_OFFSET" -r "$PRB" --numerology 0 --ssb "$SSB" \
    --rfsim \
    --rfsimulator.[0].serveraddr 127.0.0.1 \
    --time-sync-I 0.1 \
    --ntn-initial-time-drift "$TIME_DRIFT" \
    ${UE_EXTRA_ARGS:-} \
    < /dev/null > "$UE_LOG" 2>&1 &
UE_PID=$!
disown

echo "== waiting for the PDU session"
for _ in $(seq 180); do
    sudo grep -qai 'Interface oaitun_ue1 successfully configured' "$UE_LOG" 2>/dev/null && break
    sleep 1
done

if ip addr show oaitun_ue1 &>/dev/null; then
    echo "== attached"
    ip -4 addr show oaitun_ue1 | sed -n 's/.*inet \([0-9.]*\).*/   UE IP: \1/p'
    echo "== ping over the LEO link (radio round trip plus scheduling: ~100-150 ms at the horizon, falling as the satellite climbs)"
    ping -I oaitun_ue1 -c 4 -W 5 "${PING_TARGET:-10.0.0.1}" || true
else
    echo "== NOT attached. UE log tail:" >&2
    sudo tail -40 "$UE_LOG" >&2
    exit 1
fi

echo
echo "gNB log: $GNB_LOG   UE log: $UE_LOG"
echo "Ctrl-C to stop both."
# Until the UE exits or Ctrl-C (disowned, and root-owned: poll /proc rather than wait / kill -0).
while [[ -e /proc/$UE_PID ]]; do sleep 1; done
