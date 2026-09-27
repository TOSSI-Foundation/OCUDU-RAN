#!/usr/bin/env bash
# Transparent GEO NTN bring-up: OCUDU gNB + OAI nr-uesoftmodem over the rfsimulator.
# Replaces the ZMQ testbed's three-process setup (gNB, geo_buffered_relay.py, UE) with two: the rfsimulator
# applies the propagation delay itself, so there is no relay to run or keep in step.
set -euo pipefail

HERE=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)

GNB=${GNB:-$HERE/build_ntn/apps/gnb/gnb}
CFG=${CFG:-$HERE/configs/ntn/geo_rfsim_gnb.yml}
UE_CONF=ue.ntn.geo.rfsim.slice.conf
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
UECFG=${UECFG:-$OAI/targets/PROJECTS/GENERIC-NR-5GC/CONF/ue.ntn.geo.rfsim.slice.conf}

# One way, in ms. Applied on each peer's receive path, so the round trip is twice this; it must stay consistent
# with cell_specific_koffset in $CFG (240 slots at 15 kHz SCS covers a 240.7 ms round trip).
PROP_DELAY=${PROP_DELAY:-119.36926}

# Radio parameters, matching cell_cfg in $CFG: n256, 2185 MHz DL, UL 190 MHz below, 52 PRB, 15 kHz SCS.
# --ssb 62, not 60: at 52 PRB the SSB sits at offsetToPointA(5 PRB) * 12 + k_SSB(2) = 62 subcarriers from
# Point A. With the wrong value the UE never decodes PBCH and searches forever.
BAND=${BAND:-256}
DL_FREQ=${DL_FREQ:-2185000000}
UL_OFFSET=${UL_OFFSET:--190000000}
PRB=${PRB:-52}
SSB=${SSB:-62}

# The gNB writes its own log to log.filename in $CFG. The script's stdout redirect must go somewhere else:
# that file is created by root, and a non-root shell reopening it next run gets EACCES, which left the gNB
# unstarted while the NG-setup wait below matched a stale success line from the previous run.
GNB_LOG=${GNB_LOG:-/tmp/gnb_geo.log}
GNB_STDOUT=${GNB_STDOUT:-/tmp/gnb_geo_stdout.log}
UE_LOG=${UE_LOG:-/tmp/ue_geo.log}
UE_RUNDIR=${UE_RUNDIR:-/tmp/ue_geo_run}

for f in "$GNB" "$CFG" "$UE" "$UECFG"; do
    [[ -e $f ]] || { echo "missing: $f" >&2; exit 1; }
done

# A leftover UE or gNB holds the rfsimulator port and the next run fails to bind.
echo "== killing stale processes"
sudo pkill -9 -f nr-uesoftmodem || true
sudo pkill -9 -f 'apps/gnb/gnb' || true
sleep 2

# The gNB propagates the satellite position from epoch_timestamp and broadcasts the result in SIB19; the UE
# derives its whole timing advance from that. A stale epoch shifts the TA and random access fails.
echo "== refreshing epoch_timestamp"
sed -i "s/epoch_timestamp: '[^']*'/epoch_timestamp: '$(date -u '+%Y-%m-%dT%H:%M:%S')'/" "$CFG"
grep -m1 '^ *epoch_timestamp:' "$CFG"

# Drop the previous run's log: it is root-owned, which both blocks the gNB from recreating it and lets the
# NG-setup wait match a success line from an earlier run.
sudo rm -f "$GNB_LOG"
rm -f "$GNB_STDOUT"

echo "== starting gNB  (log: $GNB_LOG)"
sudo "$GNB" -c "$CFG" > "$GNB_STDOUT" 2>&1 &
trap 'sudo pkill -9 -f apps/gnb/gnb 2>/dev/null || true' EXIT

# N2 is SCTP, so a TCP probe against 38412 proves nothing. Wait for the gNB to say the procedure succeeded.
echo "== waiting for NG setup"
ng_up() { sudo grep -qaiE "NG Setup Procedure.*(finished successfully|succeeded)" "$GNB_LOG" 2>/dev/null; }
for _ in $(seq 60); do
    ng_up && break
    pgrep -f "apps/gnb/gnb" >/dev/null || { echo "gNB died:"; cat "$GNB_STDOUT"; exit 1; }
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
sudo "$UE" -O "$UECFG" \
    --band "$BAND" -C "$DL_FREQ" --CO "$UL_OFFSET" -r "$PRB" --numerology 0 --ssb "$SSB" \
    --rfsim \
    --rfsimulator.[0].serveraddr 127.0.0.1 \
    --rfsimulator.[0].prop_delay "$PROP_DELAY" ${UE_EXTRA_ARGS:-} \
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
    echo "== ping over the GEO link (expect ~500 ms RTT)"
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
