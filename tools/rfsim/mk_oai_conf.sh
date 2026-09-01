#!/usr/bin/env bash
set -eu
src=$1; dst=$2
python3 - "$src" "$dst" <<'PY'
import re, sys
src, dst = sys.argv[1], sys.argv[2]
s = open(src).read()
s = re.sub(r'snssaiList\s*=\s*\(\s*\{\s*sst\s*=\s*1;\s*\}\s*\)',
           'snssaiList = ({ sst = 1; sd = 0x000001; })', s)
s = re.sub(r'mcc\s*=\s*\d+', 'mcc = 001', s)
s = re.sub(r'mnc\s*=\s*\d+', 'mnc = 01', s)
s = re.sub(r'mnc_length\s*=\s*\d+', 'mnc_length = 2', s)
s = s.replace('192.168.80.', '192.168.70.')
i = s.find('rfsimulator :')
if i > 0:
    j = s.find('};', i)
    blk = s[i:j+2].replace('rfsimulator :', 'rfsimulator = (', 1)
    s = s[:i] + blk[:-2] + '}\n);\n' + s[j+2:]
open(dst, 'w').write(s)
PY
grep -nE "snssaiList|mcc =|amf_ip_address|rfsimulator = \(" "$dst" | head -4
