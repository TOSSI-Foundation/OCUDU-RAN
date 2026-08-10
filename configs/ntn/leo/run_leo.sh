#!/bin/bash
set -u

OCUDU_DIR=${OCUDU_DIR:-$(cd "$(dirname "${BASH_SOURCE[0]}")/../../.." && pwd)}
OAI_DIR=${OAI_DIR:-/opt/openairinterface5g}

GNB_BIN=${GNB_BIN:-$OCUDU_DIR/build/apps/gnb/gnb}
BASE_CFG=${BASE_CFG:-$OCUDU_DIR/configs/ntn/gnb_zmq_10mhz.yml}

HERE=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
NTN_CFG=${NTN_CFG:-$HERE/leo_600km_gnb.yml}
AMF_CFG=${AMF_CFG:-$HERE/leo_600km_amf.yml}
UE_CFG=${UE_CFG:-$HERE/leo_600km_ue.conf}

OUT=${OUT:-/tmp/leo_run}
GNB_SECS=${GNB_SECS:-75}
UE_SECS=${UE_SECS:-65}
UE_START_DELAY=${UE_START_DELAY:-8}
WAIT_SECS=${WAIT_SECS:-58}

INITIAL_FO=${INITIAL_FO:--6302}
TIME_DRIFT=${TIME_DRIFT:-5.77}

if [ -e "$OAI_DIR/cmake_targets/nr-uesoftmodem" ]; then
  OAI_BIN=$OAI_DIR/cmake_targets
elif [ -e "$OAI_DIR/cmake_targets/ran_build/build/nr-uesoftmodem" ]; then
  OAI_BIN=$OAI_DIR/cmake_targets/ran_build/build
else
  echo "nr-uesoftmodem not found under $OAI_DIR/cmake_targets[/ran_build/build]"
  echo "set OAI_DIR to the OpenAirInterface repository root"
  exit 1
fi

for f in "$GNB_BIN" "$BASE_CFG" "$NTN_CFG" "$AMF_CFG" "$UE_CFG" \
         "$OAI_BIN/nr-uesoftmodem" "$OAI_BIN/liboai_zmqdevif.so"; do
  [ -e "$f" ] || { echo "missing: $f"; exit 1; }
done

nm -C "$OAI_BIN/liboai_zmqdevif.so" 2>/dev/null | grep -q "ntn_orbit_model::eval_to" || {
  echo "$OAI_BIN/liboai_zmqdevif.so lacks the NTN channel model; rebuild with -DOAI_ZMQ=ON"
  exit 1
}

pkill -9 -x nr-uesoftmodem 2>/dev/null
for p in $(pgrep -x gnb 2>/dev/null); do
  case "$(readlink -f "/proc/$p/exe" 2>/dev/null)" in "$OCUDU_DIR"/*) kill -9 "$p" 2>/dev/null ;; esac
done
sleep 2

rm -rf "$OUT"; mkdir -p "$OUT/uecwd"
cp "$NTN_CFG" "$OUT/gnb.yml"
cp "$UE_CFG"  "$OUT/ue.conf"

NOW=$(date -u '+%Y-%m-%dT%H:%M:%S')
NOW_UNIX=$(date -u -d "$NOW" +%s)
sed -i "s/epoch_timestamp: '[^']*'/epoch_timestamp: '$NOW'/" "$OUT/gnb.yml"
sed -i "s/ntn_epoch      = .*/ntn_epoch      = ${NOW_UNIX}.0;/" "$OUT/ue.conf"
grep -q "epoch_timestamp: '$NOW'" "$OUT/gnb.yml" || { echo "failed to stamp gNB epoch"; exit 1; }
grep -q "ntn_epoch      = ${NOW_UNIX}.0;" "$OUT/ue.conf" || { echo "failed to stamp UE epoch"; exit 1; }

( timeout "$GNB_SECS" "$GNB_BIN" -c "$BASE_CFG" -c "$OUT/gnb.yml" -c "$AMF_CFG" \
    ${GNB_EXTRA:-} > "$OUT/gnb.log" 2>&1 & )
sleep "$UE_START_DELAY"

( cd "$OUT/uecwd" && sudo -n timeout "$UE_SECS" "$OAI_BIN/nr-uesoftmodem" \
    -O "$OUT/ue.conf" --band 256 -C 2185000000 --CO -190000000 -r 52 \
    --numerology 0 --ssb 62 --initial-fo "$INITIAL_FO" --cont-fo-comp 3 \
    --time-sync-I 0.1 --ntn-initial-time-drift "$TIME_DRIFT" \
    --device.name oai_zmqdevif ${UE_EXTRA:-} > "$OUT/ue.log" 2>&1 & )

sleep "$WAIT_SECS"
IP=$(ip -br addr show oaitun_ue1 2>/dev/null | awk '{print $3}' | cut -d/ -f1)
sleep 12

strip_ansi() { sed 's/\x1b\[[0-9;]*m//g' "$1"; }

PBCH=$(strip_ansi "$OUT/ue.log" | grep -c 'pbch decoded sucessfully')
SIB1=$(strip_ansi "$OUT/ue.log" | grep -c 'Found SIB1')
SIB19=$(strip_ansi "$OUT/ue.log" | grep -c 'Found SIB19')
PRACHTX=$(strip_ansi "$OUT/ue.log" | grep -c 'placing PRACH')
PRACHRX=$(grep -c 'PRACH: rsi' "$OUT/gnb.log")
RA=$(grep -c 'rrcSetupRequest' "$OUT/gnb.log")
NAS=$(strip_ansi "$OUT/ue.log" | grep -ci 'Registration Accept')
PACING=$(grep -o 'real-time pacing: .*' "$OUT/gnb.log" | tail -1)
PRACH_TA=$(grep -oE 'ta=[0-9.]+us' "$OUT/gnb.log" | head -4 | tr '\n' ' ')

echo
echo "RESULT pbch=$PBCH sib1=$SIB1 sib19=$SIB19 prachtx=$PRACHTX prachrx=$PRACHRX ra=$RA nas=$NAS ip=${IP:-none}"
echo "PRACH  ta: ${PRACH_TA:-none detected}"
[ -n "$PACING" ] && echo "PACING $PACING"
echo "LOGS   $OUT/gnb.log $OUT/ue.log"

[ -n "$IP" ] && exit 0 || exit 1
