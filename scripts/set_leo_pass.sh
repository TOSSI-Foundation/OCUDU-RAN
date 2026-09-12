#!/usr/bin/env bash
set -euo pipefail

HERE=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
BASE=$HERE/configs/ntn/leo_rfsim_gnb.yml
SAT1=$HERE/configs/ntn/leo_ho_sat1.yml
SAT2=$HERE/configs/ntn/leo_ho_sat2.yml

usage() {
    cat <<'USAGE'
Usage: set_leo_pass.sh --elev <deg|horizon|zenith> [options]

  --elev <deg|horizon|zenith>   where satellite 1 starts. Required unless --show.
  --elev2 <deg|horizon|zenith>  where satellite 2 starts. Default: 18.3958 deg of phase behind satellite 1.
                                Only valid with --ho / --both.
  --single                      write the single-cell config only   (configs/ntn/leo_rfsim_gnb.yml)
  --ho                          write the handover pair only        (leo_ho_sat1.yml + leo_ho_sat2.yml)
  --both                        write all three (default)
  --lead <seconds>              stamp the epoch this far ahead (default 12)
  --no-epoch                    change geometry only, leave the epoch alone
  --show                        print the current geometry and epoch, change nothing

Examples:
  ./scripts/set_leo_pass.sh --elev horizon
  ./scripts/set_leo_pass.sh --elev 40 --elev2 10 --ho
USAGE
    exit "${1:-0}"
}

ELEV=; ELEV2=; TARGET=both; LEAD=12; STAMP=1; SHOW=0
while (( $# )); do
    case $1 in
        --elev)     ELEV=${2:?--elev needs a value}; shift 2 ;;
        --elev=*)   ELEV=${1#*=}; shift ;;
        --elev2)    ELEV2=${2:?--elev2 needs a value}; shift 2 ;;
        --elev2=*)  ELEV2=${1#*=}; shift ;;
        --single)   TARGET=single; shift ;;
        --ho)       TARGET=ho;     shift ;;
        --both)     TARGET=both;   shift ;;
        --lead)     LEAD=${2:?--lead needs a value}; shift 2 ;;
        --lead=*)   LEAD=${1#*=}; shift ;;
        --no-epoch) STAMP=0; shift ;;
        --show)     SHOW=1; shift ;;
        -h|--help)  usage ;;
        *)          echo "unknown argument: $1" >&2; usage 1 ;;
    esac
done

case $ELEV  in horizon) ELEV=0  ;; zenith) ELEV=90  ;; esac
case $ELEV2 in horizon) ELEV2=0 ;; zenith) ELEV2=90 ;; esac
[[ -z $ELEV2 || $TARGET != single ]] || { echo "--elev2 needs --ho or --both" >&2; exit 1; }

if (( SHOW )); then
    for f in "$BASE" "$SAT1" "$SAT2"; do
        printf '%-22s ' "$(basename "$f")"
        SHOW_FILE=$f python3 -c "
import os,re,math
R,B=6956752.314,6356752.314
s=open(os.environ['SHOW_FILE']).read()
g=lambda k: float(re.search(rf'^\s*{k}:\s*(\S+)',s,re.M).group(1))
py,pz=g('pos_y'),g('pos_z')
ga=math.degrees(math.atan2(-py,pz))
el=math.degrees(math.atan2(R*math.cos(math.radians(ga))-B, R*math.sin(math.radians(ga)))) if ga>1e-9 else 90.0
d=math.sqrt(R*R+B*B-2*R*B*math.cos(math.radians(ga)))
print(f'elevation {el:6.2f} deg   phase f = {-ga:8.4f} deg   round trip {2*d/299792458*1e3:5.2f} ms')"
    done
    printf '%-22s %s\n' 'epoch (base config)' "$(sed -n "s/^ *epoch_timestamp: '\([^']*\)'.*/\1/p" "$BASE")"
    [[ -f /tmp/leo_epoch ]] && printf '%-22s %s\n' '/tmp/leo_epoch' "$(date -d @"$(cat /tmp/leo_epoch)" -u '+%Y-%m-%dT%H:%M:%S UTC')"
    exit 0
fi

[[ -n $ELEV ]] || { echo "--elev is required (degrees, or 'horizon' / 'zenith')" >&2; usage 1; }

EPOCH=
if (( STAMP )); then
    EPOCH=$(date -u -d "+${LEAD} seconds" '+%Y-%m-%dT%H:%M:%S')
    sed -i "s/epoch_timestamp: '[^']*'/epoch_timestamp: '$EPOCH'/" "$BASE"
    date -d "$EPOCH UTC" +%s > /tmp/leo_epoch
fi

ELEV=$ELEV ELEV2=${ELEV2:-} TARGET=$TARGET EPOCH=${EPOCH:-} LEAD=$LEAD \
BASE=$BASE SAT1=$SAT1 SAT2=$SAT2 python3 - <<'PY'
import math, os, re

R, B, V = 6956752.314, 6356752.314, 7569.47
SEP     = 18.3958
DEG_S   = 0.062342

def gamma(el_deg):
    el = math.radians(el_deg)
    return math.degrees(math.acos(B * math.cos(el) / R) - el)

def elevation(g_deg):
    if abs(g_deg) < 1e-9: return 90.0
    g = math.radians(abs(g_deg))
    return math.degrees(math.atan2(R*math.cos(g) - B, R*math.sin(g)))

def rtt_ms(g_deg):
    d = math.sqrt(R*R + B*B - 2*R*B*math.cos(math.radians(g_deg)))
    return 2*d/299792458*1e3

def write(path, g_deg):
    g = math.radians(g_deg)
    vals = (-R*math.sin(g), R*math.cos(g), V*math.cos(g), V*math.sin(g))
    z = lambda x: x + 0.0 if x else 0.0
    s = open(path).read()
    for k, v in zip(('pos_y','pos_z','vel_y','vel_z'),
                    (f'{z(vals[0]):.1f}', f'{z(vals[1]):.1f}', f'{z(vals[2]):.2f}', f'{z(vals[3]):.2f}')):
        s, n = re.subn(rf'^(\s*{k}:)\s*\S+', rf'\1 {v}', s, count=1, flags=re.M)
        assert n == 1, f'{path}: expected exactly one "{k}:" key, changed {n}'
    open(path, 'w').write(s)

elev = float(os.environ['ELEV'])
if not -90 <= elev <= 90:
    raise SystemExit('elevation must be between -90 and 90 degrees')

target = os.environ['TARGET']
g1     = gamma(elev) if elev < 90 else 0.0
epoch  = os.environ.get('EPOCH')

print(f"epoch       : {epoch + ' UTC  (+' + os.environ['LEAD'] + 's)' if epoch else '(unchanged)'}")
print(f"target      : {target}")
print()

if target in ('single', 'both'):
    write(os.environ['BASE'], g1)
    print(f"leo_rfsim_gnb.yml   single cell    elevation {elev:6.2f} deg   f = {-g1:8.4f}   round trip {rtt_ms(g1):5.2f} ms")

if target in ('ho', 'both'):
    e2raw = os.environ.get('ELEV2') or ''
    if e2raw:
        elev2 = float(e2raw)
        if not -90 <= elev2 <= 90:
            raise SystemExit('--elev2 must be between -90 and 90 degrees')
        g2 = gamma(elev2) if elev2 < 90 else 0.0
    else:
        g2 = g1 + SEP
    write(os.environ['SAT1'], g1)
    write(os.environ['SAT2'], g2)
    print(f"leo_ho_sat1.yml     handover src   elevation {elev:6.2f} deg   f = {-g1:8.4f}   round trip {rtt_ms(g1):5.2f} ms")
    print(f"leo_ho_sat2.yml     handover tgt   elevation {elevation(g2):6.2f} deg   f = {-g2:8.4f}   round trip {rtt_ms(g2):5.2f} ms")
    sep = g2 - g1
    note = '' if not e2raw else '   [--elev2, not the designed pass]'
    print(f"                    separation {sep:.4f} deg ({sep/DEG_S:.0f} s){note}")
    if sep <= 0:
        print(f"                    WARNING: satellite 2 LEADS satellite 1, so it sets first and the handover")
        print(f"                             runs backwards. Give --elev2 a LOWER elevation than --elev.")
    if elevation(g2) < 0:
        print(f"                    target below the horizon at T+0, rises about T+{-elevation(g2)/DEG_S:.0f}s (link muted)")
    print()
    print("  T+       sat1     sat2")
    for t in (0, 120, 240, 310, 415, 508):
        print(f"  {t:4d}s   {elevation(g1 - DEG_S*t):6.1f}   {elevation(g2 - DEG_S*t):6.1f}")
PY

echo
if (( STAMP )); then
    cat <<EOF
Both stacks are ready; the epoch does not need touching again.
  single cell : sudo ./build_ntn/apps/gnb/gnb -c configs/ntn/leo_rfsim_gnb.yml -c configs/ntn/sdcore.yml
  handover    : add -c configs/ntn/leo_ho_sat1.yml (and leo_ho_sat2.yml for the target)
  run scripts re-stamp by default - pass KEEP_EPOCH=1 to keep what was just set.
  wait for T+N with:
    Tplus(){ while [ \$(date +%s) -lt \$(( \$(cat /tmp/leo_epoch) + \$1 )) ]; do sleep 1; done; }
EOF
fi
