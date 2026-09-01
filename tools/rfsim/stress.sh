#!/usr/bin/env bash
set -u
LABEL=$1; PRB=$2; SSB=$3; BAND=$4; FREQ=$5; SRATE=$6; shift 6
OUT=/tmp/stress_${LABEL}.txt
: > "$OUT"
OAIB=/home/seven/fresh_oai/cmake_targets/ran_build/build
UECAP=/home/seven/fresh_oai/targets/PROJECTS/GENERIC-NR-5GC/CONF/uecap_ports1.xml
MUE=/home/seven/fresh_oai/tools/scripts/multi-ue.sh

cleanup_ues() {
  for p in $(pgrep -x nr-uesoftmodem); do kill -9 "$p" 2>/dev/null; done
  sleep 3
  for ip in $(ip rule show | grep -oP '10\.0\.0\.\d+' | sort -u); do
    while ip rule show | grep -q " $ip "; do
      ip rule del from all to "$ip" 2>/dev/null || ip rule del from "$ip" 2>/dev/null || break
    done
  done
}

launch() {
  local n=$1
  ip netns list | grep -qw "ue$n" || "$MUE" "-c$n" >/dev/null 2>&1
  ip netns exec "ue$n" bash -c "cd $OAIB && nohup setsid ./nr-uesoftmodem \
    -r $PRB --numerology 1 --band $BAND -C $FREQ --ssb $SSB \
    --rfsim --rfsimulator.serveraddr 10.$((200+n)).1.100 --rfsimulator.serverport 4043 \
    --uecap_file $UECAP --uicc0.imsi $(printf '00101%010d' "$n") \
    --uicc0.nssai_sst 1 --uicc0.nssai_sd 1 \
    --uicc0.pdu_sessions.[0].nssai_sst 1 --uicc0.pdu_sessions.[0].nssai_sd 1 \
    --uicc0.pdu_sessions.[0].dnn oai > /tmp/s_ue_$n.log 2>&1 < /dev/null &"
}

ue_ip() { ip netns exec "ue$1" ip -4 -o addr show oaitun_ue1 2>/dev/null | awk '{print $4}' | cut -d/ -f1; }

for N in "$@"; do
  cleanup_ues
  docker restart oai-ext-dn >/dev/null 2>&1; sleep 8
  for i in $(seq "$N"); do docker exec -d oai-ext-dn iperf3 -s -p $((5200+i)); done
  sleep 3

  for n in $(seq "$N"); do launch "$n"; sleep 9; done
  sleep 30

  att=0
  for n in $(seq "$N"); do
    ip=$(ue_ip "$n")
    if [ -n "$ip" ]; then att=$((att+1)); ip netns exec "ue$n" ip link set oaitun_ue1 mtu 1400; fi
  done

  for n in $(seq "$N"); do
    ip=$(ue_ip "$n")
    [ -n "$ip" ] && ip netns exec "ue$n" timeout 40 iperf3 -c 192.168.70.135 \
        -p $((5200+n)) -B "$ip" -t 12 -R -J > /tmp/s_dl_$n.json 2>&1 &
  done
  wait

  t0=$(date +%s.%N); P=$(/tmp/rfsim_probe 4043 300 2>/dev/null); t1=$(date +%s.%N)

  python3 - "$LABEL" "$N" "$att" "$P" "$t0" "$t1" "$SRATE" >> "$OUT" <<'PY'
import json, sys, re
label, n, att, out, t0, t1, srate = sys.argv[1], int(sys.argv[2]), int(sys.argv[3]), sys.argv[4], float(sys.argv[5]), float(sys.argv[6]), float(sys.argv[7])
vals = []
for i in range(1, n + 1):
    try:
        vals.append(json.load(open(f'/tmp/s_dl_{i}.json'))['end']['sum_received']['bits_per_second'] / 1e6)
    except Exception:
        vals.append(0.0)
tot = sum(vals)
live = [v for v in vals if v > 0.05]
try:
    rt = (int(re.search(r'samples=(\d+)', out).group(1)) / srate) / (t1 - t0)
except Exception:
    rt = float('nan')
lo = min(live) if live else 0.0
hi = max(live) if live else 0.0
print(f"{label} N={n:<3} attached={att}/{n:<3} carrying={len(live):<3} aggDL={tot:7.2f} Mbps "
      f"perUE={tot/max(att,1):6.2f} (min {lo:.2f} max {hi:.2f})  rt={rt:.2f}x")
PY
  tail -1 "$OUT"
done
cleanup_ues
echo DONE >> "$OUT"
