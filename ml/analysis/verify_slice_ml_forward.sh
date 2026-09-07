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
  echo "usage: bash ml/analysis/verify_slice_ml_forward.sh <path/to/model.model>" >&2
  exit 1
fi

FMT_BASE=$(find / -name base.h -path '*fmt*' 2>/dev/null | head -1 || true)
if [[ -z "$FMT_BASE" ]]; then
  echo "error: could not locate the fmt headers needed to compile the scheduler config header" >&2
  exit 1
fi
FMT_INC=$(dirname "$(dirname "$FMT_BASE")")

TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT

g++ -std=c++17 -O2 -I. -Iinclude -I"$FMT_INC" -o "$TMP/verify" ml/analysis/verify_slice_ml_forward.cpp
"$TMP/verify" "$MODEL" > "$TMP/cpp.txt"
python3 ml/analysis/verify_slice_ml_forward.py "$MODEL" > "$TMP/py.txt"

python3 - "$TMP/py.txt" "$TMP/cpp.txt" <<'PY'
import sys
import numpy as np
py = np.loadtxt(sys.argv[1], delimiter=",")
cp = np.loadtxt(sys.argv[2], delimiter=",")
if py.shape != cp.shape:
    sys.exit(f"FAIL: shape mismatch {py.shape} vs {cp.shape}")
d = np.abs(py - cp).max()
print(f"  steps compared : {py.shape[0]}   actions : {py.shape[1]}")
print(f"  max abs diff   : {d:.3e}")
print(f"  argmax match   : {bool((py.argmax(1) == cp.argmax(1)).all())}")
if d > 1e-9:
    sys.exit("FAIL: the C++ forward pass does not reproduce PyTorch")
print("PASS: C++ forward pass matches PyTorch")
PY
