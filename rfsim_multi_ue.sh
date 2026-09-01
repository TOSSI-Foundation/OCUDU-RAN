#!/usr/bin/env bash
set -euo pipefail

SELF=$(readlink -f "$0")

usage() {
    cat <<EOF
usage: $(basename "$SELF") [options]

  -n N       number of UEs                        (default 8)
  -b MHz     cell bandwidth, 40 or 100            (default 40)
  -s SEC     delay between UE launches            (default 16)
  -t SEC     iperf duration per direction         (default 15)
  -N NAME    tmux session name                    (default rfsim)
  -c LIST    CPU list to pin the UEs to           (e.g. 0-10, default none)
  -w SEC     extra time to keep waiting for attaches after the last UE has
             been launched                        (default 240)
  -E SEC     give up on the stragglers once the attach count has been
             unchanged this long                  (default 60)
  -R LIST    stress ramp: comma-separated UE counts, headless, no tmux
             (e.g. -R 8,16,24,32); results to /tmp/rfsim_stress.txt
  -o FILE    per-UE metrics csv      (default /tmp/rfsim_metrics.csv, appended)
  -O FILE    summary text file        (default /tmp/rfsim_summary_<run>.txt)
  -G FILE    gNB log to read scheduler metrics from   (default /tmp/gnb.log)
  -F FILE    diagnostics for UEs that never got a tunnel: the NAS/RRC reason,
             the tail of that UE's output and the gNB's release lines
             (default /tmp/rfsim_failed_<run>.log)
  -D ADDR    data network address: what the UEs run iperf3 and ping against.
             Default 192.168.70.135, the OAI DN container. Point it at whatever
             host runs the iperf3 servers for your core. If it is an address of this host the servers are started
             locally, so no -e is needed.
  -e NAME    data network container hosting the iperf3 servers, restarted on -k.
             Unset means the servers are expected to be running already, which is
             what a core outside this host needs. If -D is an address of this
             host the servers are started locally instead.
  -a DIR     OAI source tree, holds the UE binary and multi-ue.sh
             (default /home/seven/OAI_RAN; UECAP follows it unless set)
  -k         full reset: tmux session, all UEs, all ue* namespaces, stale ip rules,
             iperf3 servers in \$DN_CONTAINER if set, stale sweep json (gNB left running)
  -x         same as -k
  -h         this help

overrides, normally derived from -b:

  -r PRB     PRB count            -S SSB     SSB offset
  -f Hz      centre frequency     -B BAND    NR band
  -P PORT    rfsim server port
  -M MTU     tunnel MTU           -T N       UE launch retries

A UE that never attaches no longer holds up the run: the wait gives up on it
after -E seconds of no progress, the sweep skips it, -F records why it failed,
and it appears in the csv as attached=0 with dl_source=missing so it cannot
drag the averages down.

subscriber settings, passed in the environment because they belong to the core:

  IMSI_FMT   printf format for the IMSI      (default 00101%010d)
  IMSI_BASE  added to the UE index           (default 0, so ue1 is ...0000001)
  UE_KEY     subscriber key (Ki)             UE_OPC   operator key
  NSSAI_SST  slice type                      NSSAI_SD slice differentiator
  DNN        data network name               (default oai)

Start the gNB first, then:

  sudo $(basename "$SELF") -n 4 -b 100
  sudo $(basename "$SELF") -n 16 -b 40 -c 0-10
  sudo $(basename "$SELF") -R 8,16,24,32 -b 40
  sudo $(basename "$SELF") -k

against SD-Core, whose subscribers start at 001010100000001:

  sudo setsid --fork env IMSI_BASE=100000000 \\
    UE_KEY=5122250214c33e723a5dd523fc145fc0 UE_OPC=981d464c7c52eb6e5036234984ad0bcf \\
    NSSAI_SST=1 NSSAI_SD=0x010203 DNN=internet \\
    ./$(basename "$SELF") -n 8 -b 40 -c 0-10 -D 192.168.1.117

The ramp replaces tools/rfsim/stress.sh. It tears down between counts, restarts
\$DN_CONTAINER when set, waits for the attach count to settle instead of sleeping, and
appends one line per count to /tmp/rfsim_stress.txt. Set PROBE=/path/to/rfsim_probe
to have it record the real-time factor too.

This host isolates cores 0-13 (isolcpus), so nothing is scheduled there unless it is
pinned. Without -c everything lands on 14-17 and multi-UE runs are roughly half speed.
Pin the gNB too:  sudo taskset -c 11-17 ./gnb -c ...
EOF
}

case "${1:-}" in
--ue)
    MODE=ue
    UE_INDEX=$2
    shift 2
    ;;
--iperf)
    MODE=iperf
    shift
    ;;
*) MODE=main ;;
esac

N=${N:-8}
BW=${BW:-40}
STAGGER=${STAGGER:-16}
DUR=${DUR:-15}
SESSION=${SESSION:-rfsim}
PRB=${PRB:-}
SSB=${SSB:-}
FREQ=${FREQ:-3489420000}
BAND=${BAND:-78}
SRV_PORT=${SRV_PORT:-4043}
DN=${DN:-192.168.70.135}
MTU=${MTU:-1400}
RETRIES=${RETRIES:-4}
UE_CPUS=${UE_CPUS:-}
ATTACH_TIMEOUT_OPT=${ATTACH_TIMEOUT_OPT:-}
RAMP=${RAMP:-}
SETTLE=${SETTLE:-60}
PROBE=${PROBE:-/tmp/rfsim_probe}
RUN_ID=${RUN_ID:-$(date +%Y%m%d-%H%M%S)}
REPORT_TXT=${REPORT_TXT:-/tmp/rfsim_summary_${RUN_ID}.txt}
REPORT_CSV=${REPORT_CSV:-/tmp/rfsim_metrics.csv}
GNB_LOG=${GNB_LOG:-/tmp/gnb.log}
FAIL_LOG=${FAIL_LOG:-/tmp/rfsim_failed_${RUN_ID}.log}
DN_CONTAINER=${DN_CONTAINER:-}
IMSI_FMT=${IMSI_FMT:-00101%010d}
IMSI_BASE=${IMSI_BASE:-0}
UE_KEY=${UE_KEY:-}
UE_OPC=${UE_OPC:-}
NSSAI_SST=${NSSAI_SST:-1}
NSSAI_SD=${NSSAI_SD:-1}
DNN=${DNN:-oai}
STOP=no

while getopts "a:n:b:s:t:N:c:w:R:E:o:O:G:F:e:r:S:f:B:P:D:M:T:kxh" opt; do
    case $opt in
    a) OAI=$OPTARG ;;
    n) N=$OPTARG ;;
    b) BW=$OPTARG ;;
    s) STAGGER=$OPTARG ;;
    t) DUR=$OPTARG ;;
    N) SESSION=$OPTARG ;;
    c) UE_CPUS=$OPTARG ;;
    w) ATTACH_TIMEOUT_OPT=$OPTARG ;;
    R) RAMP=$OPTARG ;;
    E) SETTLE=$OPTARG ;;
    o) REPORT_CSV=$OPTARG ;;
    O) REPORT_TXT=$OPTARG ;;
    G) GNB_LOG=$OPTARG ;;
    F) FAIL_LOG=$OPTARG ;;
    e) DN_CONTAINER=$OPTARG ;;
    r) PRB=$OPTARG ;;
    S) SSB=$OPTARG ;;
    f) FREQ=$OPTARG ;;
    B) BAND=$OPTARG ;;
    P) SRV_PORT=$OPTARG ;;
    D) DN=$OPTARG ;;
    M) MTU=$OPTARG ;;
    T) RETRIES=$OPTARG ;;
    k | x) STOP=yes ;;
    h)
        usage
        exit 0
        ;;
    *)
        usage
        exit 1
        ;;
    esac
done

case $BW in
40)
    PRB=${PRB:-106}
    SSB=${SSB:-42}
    ;;
100)
    PRB=${PRB:-273}
    SSB=${SSB:-180}
    ;;
*)
    echo "bandwidth must be 40 or 100, got '$BW'; override -r and -S for anything else" >&2
    exit 1
    ;;
esac

OAI=${OAI:-/home/seven/OAI_RAN}
BUILD=$OAI/cmake_targets/ran_build/build
UECAP=${UECAP:-$OAI/targets/PROJECTS/GENERIC-NR-5GC/CONF/uecap_ports1.xml}
MULTI_UE=$OAI/tools/scripts/multi-ue.sh
ATTACH_TIMEOUT=${ATTACH_TIMEOUT_OPT:-${ATTACH_TIMEOUT:-240}}
ENV_VARS="N=$N BW=$BW UE_CPUS=$UE_CPUS STAGGER=$STAGGER DUR=$DUR SESSION=$SESSION PRB=$PRB SSB=$SSB FREQ=$FREQ \
BAND=$BAND SRV_PORT=$SRV_PORT DN=$DN MTU=$MTU RETRIES=$RETRIES OAI=$OAI UECAP=$UECAP \
ATTACH_TIMEOUT=$ATTACH_TIMEOUT PROBE=$PROBE RUN_ID=$RUN_ID \
REPORT_TXT=$REPORT_TXT REPORT_CSV=$REPORT_CSV GNB_LOG=$GNB_LOG FAIL_LOG=$FAIL_LOG \
DN_CONTAINER=$DN_CONTAINER IMSI_FMT=$IMSI_FMT IMSI_BASE=$IMSI_BASE \
UE_KEY=$UE_KEY UE_OPC=$UE_OPC NSSAI_SST=$NSSAI_SST NSSAI_SD=$NSSAI_SD DNN=$DNN"

record_failed_ues() {
    local count=$1 n ip pane alive rnti failed=0
    : >"$FAIL_LOG"
    for n in $(seq "$count"); do
        ip=$(ue_ip "$n" || true)
        [ -n "$ip" ] && continue
        failed=$((failed + 1))
        alive=$(pgrep -f "uicc0.imsi $(printf "$IMSI_FMT" "$((IMSI_BASE + n))")" | wc -l)
        {
            echo "=============================================================="
            echo "ue$n  imsi=$(printf "$IMSI_FMT" "$((IMSI_BASE + n))")  netns=ue$n"
            echo "  no tunnel at sweep time; process alive: $alive"
            out=/tmp/rfsim_faildump_$n.txt
            if command -v tmux >/dev/null 2>&1 &&
               tmux capture-pane -p -J -S -3000 -t "$SESSION.$((n - 1))" >"$out" 2>/dev/null &&
               [ -s "$out" ]; then
                :
            elif [ -f "/tmp/rfsim_ue_$n.log" ] &&
                 [ "/tmp/rfsim_ue_$n.log" -nt "$GNB_LOG" ]; then
                tail -3000 "/tmp/rfsim_ue_$n.log" >"$out" 2>/dev/null
            else
                : >"$out"
            fi
            echo "--- why it failed (nas / rrc / sync) ---"
            grep -aiE "reject|failure|authentic|identity|deregist|RRCRelease|out of sync|no cell|synch" "$out" |
                grep -av "pucch_resource_indicator" | tail -12 | cut -c1-160
            echo "--- last output from the UE ---"
            grep -av '^[[:space:]]*$' "$out" | tail -25 | cut -c1-160
            rm -f "$out"
            echo "--- gNB lines mentioning release or radio link failure ---"
            grep -aiE "release|RLF" "$GNB_LOG" 2>/dev/null | tail -10
        } >>"$FAIL_LOG"
    done
    if [ "$failed" -gt 0 ]; then
        echo "$failed of $count UEs had no tunnel; diagnostics in $FAIL_LOG"
    fi
}

start_iperf_servers() {
    local count=$1 n listening

    if [ -n "$DN_CONTAINER" ] && command -v docker >/dev/null 2>&1; then
        for n in $(seq "$count"); do
            docker exec -d "$DN_CONTAINER" iperf3 -s -p "$((5200 + n))" >/dev/null 2>&1 || true
        done
        sleep 3
    elif ip -4 -o addr show | grep -qw "$DN"; then
        echo "starting $count local iperf3 servers ($DN is an address of this host)"
        for n in $(seq "$count"); do
            pgrep -f "iperf3 -s -p $((5200 + n))\b" >/dev/null 2>&1 && continue
            setsid --fork iperf3 -s -p "$((5200 + n))" >/dev/null 2>&1 </dev/null
        done
        sleep 3
    fi

    listening=$(ss -tln 2>/dev/null | grep -cE ":$((5200 + 1))\b|:$((5200 + count))\b")
    if [ "$listening" -eq 0 ] && ! (exec 3<>"/dev/tcp/$DN/5201") 2>/dev/null; then
        echo "WARNING: nothing is listening on $DN:5201..$((5200 + count))."
        echo "         The sweep will report 0.00 Mbps for every UE. Start the servers"
        echo "         on the data network first, or pass -e <container> to have this"
        echo "         script run them for you."
    fi
}

ue_ip() {
    ip netns exec "ue$1" ip -4 -o addr show oaitun_ue1 2>/dev/null | awk '{print $4}' | cut -d/ -f1
}

cpu_for_ue() {
    local n=$1 list=() c lo hi part
    [ -z "$UE_CPUS" ] && return 1
    IFS=, read -ra parts <<<"$UE_CPUS"
    for part in "${parts[@]}"; do
        if [[ $part == *-* ]]; then
            lo=${part%%-*}; hi=${part##*-}
            for ((c = lo; c <= hi; c++)); do list+=("$c"); done
        else
            list+=("$part")
        fi
    done
    [ ${#list[@]} -eq 0 ] && return 1
    echo "${list[$(((n - 1) % ${#list[@]}))]}"
}

run_ue() {
    local n=$1 attempt=0 pin=() core
    sleep $(((n - 1) * STAGGER))
    cd "$BUILD"
    if core=$(cpu_for_ue "$n"); then
        pin=(taskset -c "$core")
        echo "ue$n pinned to cpu $core"
    fi
    while [ "$attempt" -lt "$RETRIES" ]; do
        attempt=$((attempt + 1))
        local cred=()
        [ -n "$UE_KEY" ] && cred+=(--uicc0.key "$UE_KEY")
        [ -n "$UE_OPC" ] && cred+=(--uicc0.opc "$UE_OPC")
        "${pin[@]}" ./nr-uesoftmodem -r "$PRB" --numerology 1 --band "$BAND" -C "$FREQ" --ssb "$SSB" \
            --rfsim --rfsimulator.serveraddr "10.$((200 + n)).1.100" --rfsimulator.serverport "$SRV_PORT" \
            --uecap_file "$UECAP" \
            --uicc0.imsi "$(printf "$IMSI_FMT" "$((IMSI_BASE + n))")" \
            "${cred[@]}" \
            --uicc0.nssai_sst "$NSSAI_SST" --uicc0.nssai_sd "$NSSAI_SD" \
            --uicc0.pdu_sessions.[0].nssai_sst "$NSSAI_SST" --uicc0.pdu_sessions.[0].nssai_sd "$NSSAI_SD" \
            --uicc0.pdu_sessions.[0].dnn "$DNN" && break
        echo "ue$n exited on attempt $attempt of $RETRIES, retrying"
        sleep 5
    done
}

summarise() {
    python3 - "$1" "$N" <<'PY'
import json, sys
direction, n = sys.argv[1], int(sys.argv[2])
key = 'sum_received' if direction == 'dl' else 'sum_sent'
total = 0.0
for i in range(1, n + 1):
    try:
        with open(f'/tmp/rfsim_{direction}_{i}.json') as f:
            mbps = json.load(f)['end'][key]['bits_per_second'] / 1e6
    except Exception:
        print(f'  ue{i:<3}   no result')
        continue
    total += mbps
    print(f'  ue{i:<3} {mbps:8.2f} Mbps')
print(f'  {direction.upper()} aggregate {total:.2f} Mbps over {n} UEs ({total / n:.2f} per UE)')
PY
}

sweep() {
    local dir=$1 flag=$2 n ip
    echo
    echo "=== $dir, $DUR s, $N UEs concurrently, $BW MHz ==="
    for n in $(seq "$N"); do
        ip=$(ue_ip "$n" || true)
        if [ -z "$ip" ]; then
            echo "  ue$n no tunnel, skipped"
            continue
        fi
        ip netns exec "ue$n" iperf3 -c "$DN" -p "$((5200 + n))" -B "$ip" -t "$DUR" $flag -J \
            >"/tmp/rfsim_${dir}_${n}.json" 2>"/tmp/rfsim_${dir}_${n}.err" &
    done
    wait
    summarise "$dir"
}

write_report() {
    local rt=$1 rtt=$2 t0=${3:-} t1=${4:-} reporter
    reporter="$(dirname "$SELF")/tools/rfsim/rfsim_report.py"
    [ -f "$reporter" ] || { echo "no reporter at $reporter"; return 0; }
    IMSI_FMT="$IMSI_FMT" IMSI_BASE="$IMSI_BASE" python3 "$reporter" "$N" "$BW" "$PRB" "$SSB" "$BAND" "$FREQ" "$UE_CPUS" \
        "$DUR" "$STAGGER" "$rt" "$rtt" "$REPORT_TXT" "$REPORT_CSV" "$RUN_ID" \
        "$GNB_LOG" "$t0" "$t1" || true
}

run_iperf() {
    local n waited
    echo "waiting for $N tunnels, $BW MHz, $PRB PRB, ssb $SSB"
    local launched=$(((N - 1) * STAGGER + 30))
    local att=0 prev=-1 still=0
    waited=0
    while [ "$waited" -lt "$((launched + ATTACH_TIMEOUT))" ]; do
        att=0
        for n in $(seq "$N"); do [ -n "$(ue_ip "$n" || true)" ] && att=$((att + 1)); done
        [ "$att" -ge "$N" ] && break
        if [ "$waited" -ge "$launched" ]; then
            if [ "$att" -eq "$prev" ]; then
                still=$((still + 2))
                if [ "$still" -ge "$SETTLE" ]; then
                    echo "attach count held at $att/$N for ${SETTLE}s, going on without the rest"
                    break
                fi
            else
                still=0
                prev=$att
            fi
        fi
        sleep 2
        waited=$((waited + 2))
    done

    for n in $(seq "$N"); do
        if [ -n "$(ue_ip "$n" || true)" ]; then
            ip netns exec "ue$n" ip link set oaitun_ue1 mtu "$MTU"
        fi
        printf 'ue%-3s %s\n' "$n" "$(ue_ip "$n" || echo none)"
    done

    record_failed_ues "$N"
    start_iperf_servers "$N"

    local sweep_start sweep_end
    sweep_start=$(date +%Y-%m-%dT%H:%M:%S)
    sweep dl -R
    sleep 5
    sweep ul ""
    sweep_end=$(date +%Y-%m-%dT%H:%M:%S)

    local rt="" t0 t1
    if [ -x "$PROBE" ]; then
        t0=$(date +%s.%N)
        "$PROBE" "$SRV_PORT" 300 >/dev/null 2>&1 || true
        t1=$(date +%s.%N)
        rt=$(python3 -c "print(f'{(300*0.0005)/($t1-$t0):.4f}')" 2>/dev/null || echo "")
    fi

    local rtt="" n_first
    for n_first in $(seq "$N"); do
        if [ -n "$(ue_ip "$n_first" || true)" ]; then
            rtt=$(ip netns exec "ue$n_first" ping -c 10 -i 0.3 -W 5 -I oaitun_ue1 "$DN" 2>/dev/null |
                  awk -F'/' '/rtt|round-trip/ {printf "%.2f", $5}')
            break
        fi
    done

    write_report "$rt" "$rtt" "$sweep_start" "$sweep_end"

    echo
    echo "summary : $REPORT_TXT"
    echo "per-UE  : $REPORT_CSV"
    [ -s "$FAIL_LOG" ] && echo "failures: $FAIL_LOG"
    echo "done, press enter to close"
    read -r
}

stop() {
    tmux kill-session -t "$SESSION" 2>/dev/null || true
    pkill -9 -f "$SELF --ue" 2>/dev/null || true
    sleep 1
    pkill -9 -x nr-uesoftmodem 2>/dev/null || true
    sleep 2
    local ns
    for ns in $(ip netns list | awk '{print $1}' | grep '^ue[0-9]*$'); do
        "$MULTI_UE" "-d${ns#ue}" >/dev/null 2>&1 || ip netns delete "$ns" 2>/dev/null || true
    done
    for addr in $(ip rule show | grep -oP '10\.0\.0\.\d+' | sort -u); do
        while ip rule show | grep -q " $addr "; do
            ip rule del from all to "$addr" 2>/dev/null || ip rule del from "$addr" 2>/dev/null || break
        done
    done
    local dn_reset="skipped"
    if [ -n "$DN_CONTAINER" ] && command -v docker >/dev/null 2>&1 \
       && docker inspect "$DN_CONTAINER" >/dev/null 2>&1; then
        docker restart "$DN_CONTAINER" >/dev/null 2>&1 && dn_reset="restarted" || dn_reset="failed"
    fi
    rm -f /tmp/rfsim_dl_*.json /tmp/rfsim_ul_*.json 2>/dev/null || true
    local left
    left=$(ip netns list | grep -c '^ue' || true)
    echo "killed: tmux session, all UEs, namespaces (${left:-0} left), stale ip rules cleared"
    echo "        stale iperf json removed, data network $dn_reset (gNB left running)"
}

ramp_one() {
    local n ip att prev=-1 still=0 waited=0 rt="n/a"

    for n in $(seq "$N"); do
        ip netns list | grep -qw "ue$n" || "$MULTI_UE" "-c$n" >/dev/null
    done
    for n in $(seq "$N"); do
        setsid --fork ip netns exec "ue$n" env $ENV_VARS bash "$SELF" --ue "$n" \
            >"/tmp/rfsim_ue_$n.log" 2>&1 </dev/null
    done

    local launched=$(((N - 1) * STAGGER + 30))
    local deadline=$((launched + ATTACH_TIMEOUT))
    while [ "$waited" -lt "$deadline" ]; do
        att=0
        for n in $(seq "$N"); do [ -n "$(ue_ip "$n" || true)" ] && att=$((att + 1)); done
        [ "$att" -ge "$N" ] && break
        if [ "$waited" -ge "$launched" ]; then
            if [ "$att" -eq "$prev" ]; then
                still=$((still + 5))
                [ "$still" -ge "$SETTLE" ] && break
            else
                still=0
                prev=$att
            fi
        fi
        sleep 5
        waited=$((waited + 5))
    done

    att=0
    for n in $(seq "$N"); do
        ip=$(ue_ip "$n" || true)
        if [ -n "$ip" ]; then
            att=$((att + 1))
            ip netns exec "ue$n" ip link set oaitun_ue1 mtu "$MTU" 2>/dev/null || true
        fi
    done

    record_failed_ues "$N"
    start_iperf_servers "$N"

    sweep dl "-R" >/dev/null

    local rtt="n/a" pu
    for n in $(seq "$N"); do
        if [ -n "$(ue_ip "$n" || true)" ]; then
            pu=$(ip netns exec "ue$n" ping -c 10 -i 0.3 -W 5 -I oaitun_ue1 "$DN" 2>/dev/null |
                 awk -F'/' '/rtt|round-trip/ {printf "%.1f", $5}')
            [ -n "$pu" ] && rtt="${pu}ms"
            break
        fi
    done

    if [ -x "$PROBE" ]; then
        local t0 t1
        t0=$(date +%s.%N)
        "$PROBE" "$SRV_PORT" 300 >/dev/null 2>&1 || true
        t1=$(date +%s.%N)
        rt=$(python3 -c "print(f'{(300*0.0005)/($t1-$t0):.3f}x')" 2>/dev/null || echo "n/a")
    fi

    python3 - "$N" "$att" "$rt" "$BW" "$rtt" <<'PY2'
import json, sys
n, att, rt, bw, rtt = int(sys.argv[1]), int(sys.argv[2]), sys.argv[3], sys.argv[4], sys.argv[5]
vals, failed = [], 0
for i in range(1, n + 1):
    try:
        with open(f'/tmp/rfsim_dl_{i}.json') as f:
            vals.append(json.load(f)['end']['sum_received']['bits_per_second'] / 1e6)
    except Exception:
        failed += 1
live = [v for v in vals if v > 0.05]
tot = sum(live)
rng = f'{min(live):.2f}-{max(live):.2f}' if live else 'n/a'
print(f'{bw}MHz N={n:<3} attached={att}/{n:<3} carrying={len(live):<3} '
      f'aggDL={tot:7.2f} Mbps perUE={tot/max(len(live),1):6.2f} ({rng})  rt={rt} rtt={rtt}'
      + (f'  [{failed} no result]' if failed else ''))
PY2
}

ramp() {
    local out=${STRESS_OUT:-/tmp/rfsim_stress.txt} count line
    : >"$out"
    echo "ramp: ${RAMP//,/ } UEs, $BW MHz, $PRB PRB, ssb $SSB -> $out"
    [ -x "$PROBE" ] || echo "note: no probe at $PROBE, real-time factor will be n/a"
    for count in ${RAMP//,/ }; do
        N=$count
        ENV_VARS="N=$N BW=$BW UE_CPUS=$UE_CPUS STAGGER=$STAGGER DUR=$DUR SESSION=$SESSION PRB=$PRB SSB=$SSB FREQ=$FREQ BAND=$BAND SRV_PORT=$SRV_PORT DN=$DN MTU=$MTU RETRIES=$RETRIES OAI=$OAI UECAP=$UECAP ATTACH_TIMEOUT=$ATTACH_TIMEOUT PROBE=$PROBE RUN_ID=$RUN_ID \
REPORT_TXT=$REPORT_TXT REPORT_CSV=$REPORT_CSV GNB_LOG=$GNB_LOG FAIL_LOG=$FAIL_LOG \
DN_CONTAINER=$DN_CONTAINER IMSI_FMT=$IMSI_FMT IMSI_BASE=$IMSI_BASE \
UE_KEY=$UE_KEY UE_OPC=$UE_OPC NSSAI_SST=$NSSAI_SST NSSAI_SD=$NSSAI_SD DNN=$DNN"
        stop >/dev/null 2>&1 || true
        sleep 5
        ss -tln | grep -q ":$SRV_PORT " || {
            echo "gNB stopped listening on $SRV_PORT, aborting ramp" | tee -a "$out"
            return 1
        }
        line=$(ramp_one)
        echo "$line" | tee -a "$out"
    done
    stop >/dev/null 2>&1 || true
    echo "ramp complete: $out"
}

start() {
    [ "$(id -u)" -eq 0 ] || {
        echo "run as root: sudo $SELF ..."
        exit 1
    }
    command -v tmux >/dev/null || {
        echo "tmux is not installed"
        exit 1
    }
    [ -x "$BUILD/nr-uesoftmodem" ] || {
        echo "no nr-uesoftmodem at $BUILD"
        exit 1
    }
    [ -x "$MULTI_UE" ] || {
        echo "no multi-ue.sh at $MULTI_UE"
        exit 1
    }
    ss -tln | grep -q ":$SRV_PORT " || {
        echo "nothing is listening on $SRV_PORT, start the gNB first"
        exit 1
    }

    echo "$N UEs, $BW MHz, $PRB PRB, ssb $SSB, band $BAND, $FREQ Hz"

    for n in $(seq "$N"); do
        ip netns list | grep -qw "ue$n" || "$MULTI_UE" "-c$n" >/dev/null
    done

    tmux kill-session -t "$SESSION" 2>/dev/null || true
    tmux new-session -d -s "$SESSION" -n ue "ip netns exec ue1 env $ENV_VARS bash '$SELF' --ue 1"
    for n in $(seq 2 "$N"); do
        tmux split-window -t "$SESSION" "ip netns exec ue$n env $ENV_VARS bash '$SELF' --ue $n"
        tmux select-layout -t "$SESSION" tiled >/dev/null
    done
    tmux split-window -t "$SESSION" "env $ENV_VARS bash '$SELF' --iperf"
    tmux select-layout -t "$SESSION" tiled >/dev/null
    tmux set-option -t "$SESSION" remain-on-exit on >/dev/null

    if [ -t 0 ]; then
        tmux attach -t "$SESSION"
    else
        echo "session '$SESSION' started: tmux attach -t $SESSION"
    fi
}

case $MODE in
ue) run_ue "$UE_INDEX" ;;
iperf) run_iperf ;;
main)
    if [ "$STOP" = yes ]; then
        stop
    elif [ -n "$RAMP" ]; then
        [ "$(id -u)" -eq 0 ] || {
            echo "run as root: sudo $SELF ..."
            exit 1
        }
        ss -tln | grep -q ":$SRV_PORT " || {
            echo "nothing is listening on $SRV_PORT, start the gNB first"
            exit 1
        }
        ramp
    else
        start
    fi
    ;;
esac
