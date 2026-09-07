#!/usr/bin/env bash
# Copyright 2025-2026 coRAN LABS Private Limited
#
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy of the License at
#
#     http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
# See the License for the specific language governing permissions and
# limitations under the License.

set -euo pipefail
cd "$(dirname "$0")/../.."

MODEL="${1:-}"
if [[ -z "$MODEL" || ! -f "$MODEL" ]]; then
  echo "usage: bash ml/analysis/verify_attention_forward.sh <path/to/model.model>" >&2
  exit 1
fi

TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT

g++ -std=c++17 -O2 -I. -o "$TMP/verify" ml/analysis/verify_attention_forward.cpp
"$TMP/verify" "$MODEL" > "$TMP/cpp.txt"
python3 ml/analysis/verify_attention_forward.py "$MODEL" > "$TMP/py.txt"

python3 - "$TMP/py.txt" "$TMP/cpp.txt" <<'PY'
import sys
import numpy as np
py = np.loadtxt(sys.argv[1], delimiter=",")
cp = np.loadtxt(sys.argv[2], delimiter=",")
if py.shape != cp.shape:
    sys.exit(f"FAIL: shape mismatch {py.shape} vs {cp.shape}")
both_inf = np.isneginf(py) & np.isneginf(cp)
if (np.isneginf(py) != np.isneginf(cp)).any():
    sys.exit("FAIL: the two sides disagree about which pairs are masked")
finite = ~both_inf
d = np.abs(py[finite] - cp[finite]).max()
am_ok = all(np.nanargmax(np.where(finite[r], py[r], -np.inf)) ==
            np.nanargmax(np.where(finite[r], cp[r], -np.inf)) for r in range(py.shape[0]))
print(f"  decode steps compared : {py.shape[0]}   pairs : {py.shape[1]}")
print(f"  masked pairs agree    : True")
print(f"  argmax agrees         : {am_ok}")
print(f"  max abs diff          : {d:.3e}")
if d > 5e-3 or not am_ok:
    sys.exit("FAIL: the C++ float32 forward pass does not reproduce PyTorch")
print("PASS: C++ attention forward pass matches PyTorch (float32 tolerance)")
PY
