#!/usr/bin/env python3
"""Train the SVR-RFF BSR-periodicity model on captured dataset CSVs.

Exports a compiled-in C++ seed header (--inc-out) and/or a hot-reloadable model blob (--model-out).

Usage:
    python3 ml/training/train_bsr_periodicity.py ml/datasets/*.csv --inc-out ... --model-out ...
"""

import argparse
import csv
import glob
import json
import math
import os
import sys
from collections import defaultdict

os.environ.setdefault("OMP_NUM_THREADS", "4")
import numpy as np
from sklearn.kernel_approximation import RBFSampler
from sklearn.pipeline import Pipeline
from sklearn.preprocessing import StandardScaler
from sklearn.svm import SVR

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
from bsr_ml_config import (
    REQUIRED_COLUMNS,
    TRIGGER_TYPE_REGULAR,
    VALID_PERIODIC_BSR_TIMER_SUBFRAMES,
    map_to_nearest_periodicity,
    slots_to_subframes,
)


def load_session(path):
    """Load one dataset CSV into {(rnti, ue_index): [(slot, numerology, interarrival_slots), ...]}.

    Keeps only regular BSRs (TS 38.321 clause 5.4.5) with has_interarrival == 1; periodic/padding BSRs are
    not new-data arrivals and would pollute the interarrival series.
    """
    per_ue = defaultdict(list)
    with open(path, newline="") as fh:
        reader = csv.DictReader(fh)
        missing = [c for c in REQUIRED_COLUMNS if c not in (reader.fieldnames or [])]
        if missing:
            sys.exit(
                f"{path}: missing required column(s) {missing} — this CSV predates the BSR trigger-classifier "
                f"instrumentation (lib/scheduler/logging/bsr_ml_dataset_logger.cpp). Re-capture with the "
                f"rebuilt DU."
            )
        skipped = 0
        for row in reader:
            # A capture killed mid-write leaves a truncated final line; skip rows with missing/blank fields.
            needed = ("bsr_trigger_type", "has_interarrival", "rnti", "ue_index", "slot", "numerology",
                      "interarrival_slots")
            if any(row.get(c) in (None, "") for c in needed):
                skipped += 1
                continue
            if int(row["bsr_trigger_type"]) != TRIGGER_TYPE_REGULAR:
                continue
            if int(row["has_interarrival"]) != 1:
                continue
            key = (row["rnti"], row["ue_index"])
            per_ue[key].append(
                (int(row["slot"]), int(row["numerology"]), float(row["interarrival_slots"]))
            )
    if skipped:
        print(f"  {path}: skipped {skipped} truncated/malformed row(s)", file=sys.stderr)
    return per_ue


def build_windows(values, window_size):
    """Sliding windows: X[i] is the window_size values preceding values[i + window_size]; y[i] is that value."""
    n = len(values)
    if n <= window_size:
        return np.empty((0, window_size)), np.empty((0,))
    x = np.array([values[i : i + window_size] for i in range(n - window_size)], dtype=np.float64)
    y = np.array([values[i + window_size] for i in range(n - window_size)], dtype=np.float64)
    return x, y


def build_dataset(sessions, window_size):
    """Build (X, y) across all sessions/UEs. Windows never span a session or UE boundary."""
    xs, ys = [], []
    for per_ue in sessions:
        for _key, rows in per_ue.items():
            values = [r[2] for r in rows]
            x, y = build_windows(values, window_size)
            if len(y):
                xs.append(x)
                ys.append(y)
    if not xs:
        return np.empty((0, window_size)), np.empty((0,))
    return np.concatenate(xs, axis=0), np.concatenate(ys, axis=0)


def time_split(x, y, test_frac):
    """Chronological train/test split (default 70/30). Not shuffled — the data is a time series."""
    cut = int(len(y) * (1.0 - test_frac))
    return x[:cut], y[:cut], x[cut:], y[cut:]


def _nearest_rank(sorted_errors, rank_fraction_numerator, rank_fraction_denominator):
    """Nearest-rank order statistic: the (k * (N+1) / m)-th ordered sample, rounded and clamped to [1, N]."""
    n = len(sorted_errors)
    if n == 0:
        return float("nan")
    rank = round(rank_fraction_numerator * (n + 1) / rank_fraction_denominator)
    rank = max(1, min(n, rank))
    return float(sorted_errors[rank - 1])


def evaluate(y_true, y_pred):
    errors = np.abs(y_true - y_pred)
    sorted_errors = np.sort(errors)
    mae = float(np.mean(errors)) if len(errors) else float("nan")
    median = _nearest_rank(sorted_errors, 1, 2)
    p75 = _nearest_rank(sorted_errors, 3, 4)
    p95 = _nearest_rank(sorted_errors, 19, 20)
    return {"mae": mae, "median": median, "p75": p75, "p95": p95, "n": len(errors)}


def train_svr_rff(xtr, ytr, gamma, n_components, svr_c, svr_epsilon):

    model = Pipeline(
        [
            ("scaler", StandardScaler()),
            ("rff", RBFSampler(gamma=gamma, n_components=n_components, random_state=0)),
            ("svr", SVR(kernel="linear", C=svr_c, epsilon=svr_epsilon)),
        ]
    )
    model.fit(xtr, ytr)
    return model


def _model_arrays(model):
    scaler = model.named_steps["scaler"]
    rff = model.named_steps["rff"]
    svr = model.named_steps["svr"]
    return {
        "n_components": int(rff.n_components),
        "gamma": float(rff.gamma if not isinstance(rff.gamma, str) else rff._gamma),
        # StandardScaler applied per-feature (window position) before the RFF map: x' = (x - mean) / scale.
        "scaler_mean": scaler.mean_.ravel().tolist(),
        "scaler_scale": scaler.scale_.ravel().tolist(),
        # random_weights_ shape: (window_size, n_components), row-major flatten (row i = feature i's weights).
        "random_weights": rff.random_weights_.ravel(order="C").tolist(),
        "random_offset": rff.random_offset_.ravel().tolist(),
        "coef": svr.coef_.ravel().tolist(),
        "intercept": float(svr.intercept_[0]),
    }


def export_inc(model, window_size, out_path, meta):
    """Export the fitted pipeline as a compiled-in C++ seed header, so inference works with no model file."""
    a = _model_arrays(model)

    def fmt_dbl_array(name, arr):
        return f"inline constexpr double {name}[{len(arr)}] = {{{','.join(repr(x) for x in arr)}}};"

    header = '''// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.'''
    lines = header.split("\n")
    lines.append("")
    lines.append("#pragma once")
    lines.append("namespace ocudu {")
    lines.append("namespace bsr_ml {")
    lines.append(f"inline constexpr unsigned SEED_WINDOW_SIZE  = {int(window_size)};")
    lines.append(f"inline constexpr unsigned SEED_N_COMPONENTS = {a['n_components']};")
    lines.append(f"inline constexpr double   SEED_GAMMA        = {a['gamma']!r};")
    lines.append(f"inline constexpr double   SEED_INTERCEPT    = {a['intercept']!r};")
    lines.append(fmt_dbl_array("SEED_SCALER_MEAN", a["scaler_mean"]))
    lines.append(fmt_dbl_array("SEED_SCALER_SCALE", a["scaler_scale"]))
    lines.append(fmt_dbl_array("SEED_RANDOM_WEIGHTS", a["random_weights"]))
    lines.append(fmt_dbl_array("SEED_RANDOM_OFFSET", a["random_offset"]))
    lines.append(fmt_dbl_array("SEED_COEF", a["coef"]))
    lines.append("}")
    lines.append("}")
    with open(out_path, "w") as fh:
        fh.write("\n".join(lines) + "\n")


def export_model_file(model, window_size, path, meta):
    """Export the fitted pipeline as the optional hot-swappable .model override (JSON content)."""
    a = _model_arrays(model)
    blob = {
        "format": "ocudu_bsr_periodicity_rff_svr",
        "version": 2,  # v2 adds scaler_mean/scaler_scale (StandardScaler prepended to the pipeline)
        "window_size": window_size,
        **a,
        "meta": meta,
    }
    with open(path, "w") as fh:
        json.dump(blob, fh)


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("csv", nargs="*", help="dataset CSV(s); default: ml/datasets/bsr_ml_dataset_*.csv")
    ap.add_argument("--window-size", type=int, default=30, help="sliding window size (default: 30)")
    ap.add_argument("--test-frac", type=float, default=0.3, help="chronological test split fraction (default 0.3)")
    ap.add_argument("--gamma", type=float, default=1.0, help="RBFSampler gamma (default: 1)")
    ap.add_argument("--n-components", type=int, default=100, help="RBFSampler n_components (default: 100)")
    ap.add_argument("--svr-c", type=float, default=100.0, help="SVR regularization C (default: 100)")
    ap.add_argument("--svr-epsilon", type=float, default=0.1, help="SVR epsilon (default: 0.1)")
    ap.add_argument(
        "--inc-out",
        default=None,
        help="path to write the compiled-in C++ seed header (bsr_periodicity_model.inc), baked into the DU "
        "binary so inference works out of the box; mirrors the MCS pipeline's --out",
    )
    ap.add_argument(
        "--model-out",
        default=None,
        help="path to write the runtime override model (.model, JSON content, custom extension) hot-loaded at "
        "startup if configured; mirrors the MCS pipeline's --model-out",
    )
    ap.add_argument("--joblib-out", default=None, help="optional: save the sklearn pipeline (joblib) for Python reuse")
    ap.add_argument("--min-samples", type=int, default=200, help="minimum windowed samples required to train")
    args = ap.parse_args()

    paths = args.csv or sorted(glob.glob("ml/datasets/bsr_ml_dataset_*.csv"))
    if not paths:
        sys.exit("no dataset CSVs found (pass paths explicitly, or check ml/datasets/bsr_ml_dataset_*.csv)")

    sessions = [load_session(p) for p in paths]
    x, y = build_dataset(sessions, args.window_size)
    print(
        f"files={len(paths)} ues={sum(len(s) for s in sessions)} "
        f"windowed_samples={len(y)} window_size={args.window_size}"
    )
    if len(y) < args.min_samples:
        sys.exit(
            f"not enough windowed samples: {len(y)} < --min-samples={args.min_samples} "
            f"(need more regular-classified BSR events per UE; see class docs in "
            f"lib/scheduler/logging/bsr_ml_dataset_logger.h for why most rows are filtered out)"
        )

    xtr, ytr, xte, yte = time_split(x, y, args.test_frac)
    if len(yte) == 0:
        sys.exit("test split is empty — increase samples or lower --test-frac")

    model = train_svr_rff(xtr, ytr, args.gamma, args.n_components, args.svr_c, args.svr_epsilon)
    ypred = model.predict(xte)
    metrics = evaluate(yte, ypred)
    print(
        f"held-out (chronological split, test={metrics['n']}): "
        f"MAE={metrics['mae']:.2f} slots  median={metrics['median']:.2f} slots  "
        f"p75={metrics['p75']:.2f} slots  p95={metrics['p95']:.2f} slots"
    )

    # Display-only demo. Predictions are in slots; the periodicBSR-Timer set is in subframes, so a
    # representative numerology is assumed here. A live deployment must use the scheduled UE's own numerology.
    demo_numerology = int(next(iter(sessions[0].values()))[0][1]) if sessions and sessions[0] else 1
    print(f"\nSample periodicity mapping demo (numerology={demo_numerology}, first 5 held-out predictions):")
    for i in range(min(5, len(ypred))):
        predicted_subframes = slots_to_subframes(float(ypred[i]), demo_numerology)
        mapped = map_to_nearest_periodicity(predicted_subframes)
        print(
            f"  predicted_interarrival={ypred[i]:.1f} slots ({predicted_subframes:.2f} sf) "
            f"-> nearest periodicBSR-Timer = sf{mapped}"
        )
    print(f"\nValid periodicBSR-Timer collection (TS 38.331): {VALID_PERIODIC_BSR_TIMER_SUBFRAMES}")

    if args.inc_out or args.model_out or args.joblib_out:
        # Retrain on all data for the exported model; the held-out split above is for reported metrics only.
        final_model = train_svr_rff(x, y, args.gamma, args.n_components, args.svr_c, args.svr_epsilon)
        meta = {
            "sources": [os.path.basename(p) for p in paths],
            "windowed_samples": len(y),
            "held_out_mae_slots": metrics["mae"],
            "held_out_median_slots": metrics["median"],
            "held_out_p75_slots": metrics["p75"],
            "held_out_p95_slots": metrics["p95"],
        }

        if args.inc_out:
            if args.inc_out.endswith(".model") or args.inc_out.endswith(".json"):
                sys.exit(f"refusing to write a generated C++ header to a data path: {args.inc_out}")
            export_inc(final_model, args.window_size, args.inc_out, meta)
            print(f"\nsaved compiled-in seed header -> {args.inc_out}")

        if args.model_out:
            export_model_file(final_model, args.window_size, args.model_out, meta)
            print(f"saved runtime override model -> {args.model_out}")

        if args.joblib_out:
            import joblib

            joblib.dump(final_model, args.joblib_out)
            print(f"saved sklearn pipeline (joblib) -> {args.joblib_out}")


if __name__ == "__main__":
    main()
