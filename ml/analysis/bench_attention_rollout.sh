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
  echo "usage: bash ml/analysis/bench_attention_rollout.sh <path/to/model.model> [reps]" >&2
  exit 1
fi

TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT

g++ -std=c++17 -O3 -DNDEBUG -march=native -I. -o "$TMP/bench" ml/analysis/bench_attention_rollout.cpp
"$TMP/bench" "$MODEL" "${2:-200}"
