#!/usr/bin/env python3
import argparse
import csv
import glob
import json
import math
import os
import shutil
import sys
import time

os.environ.setdefault("OMP_NUM_THREADS", "4")
import numpy as np
from sklearn.metrics import brier_score_loss, roc_auc_score

_ML_DIR = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, _ML_DIR)
sys.path.insert(0, os.path.join(_ML_DIR, "training"))
from ml_config import FEATURES, LABEL, load as load_cfg
import train_export_mcs as T


def fnum(x):
    try:
        return float(x)
    except (TypeError, ValueError):
        return math.nan


def load_newtx(data_dir):
    rows = []
    need = FEATURES + ["nof_retxs", LABEL, "tbs_bytes"]
    for p in sorted(glob.glob(os.path.join(data_dir, "ul_mcs_dataset_*.csv"))):
        with open(p, newline="") as fh:
            for r in csv.DictReader(fh):
                if any(r.get(c) in (None, "") for c in need):
                    continue
                try:
                    if int(float(r["nof_retxs"])) != 0:
                        continue
                except ValueError:
                    continue
                rows.append(r)
    return rows


def matrix(rows):
    x = np.array([[fnum(r[c]) for c in FEATURES] for r in rows], dtype=np.float64)
    y = np.array([int(float(r[LABEL])) for r in rows], dtype=np.int32)
    return x, y


def predict_blob(blob, x):
    off = np.array(blob["tree_offset"])
    nf = np.array(blob["node_feature"])
    th = np.array(blob["node_threshold"])
    nl = np.array(blob["node_left"])
    nr = np.array(blob["node_right"])
    nv = np.array(blob["node_value"])
    score = np.full(len(x), blob["init_logodds"], dtype=np.float64)
    for t in range(blob["num_trees"]):
        node = np.full(len(x), off[t], dtype=np.int64)
        while (nl[node] >= 0).any():
            idx = np.where(nl[node] >= 0)[0]
            n = node[idx]
            node[idx] = np.where(x[idx, nf[n]] <= th[n], nl[n], nr[n])
        score += blob["learning_rate"] * nv[node]
    return 1.0 / (1.0 + np.exp(-score))


def validate(candidate, incumbent, val_rows, floor_bler, slack_auc=0.01, slack_brier=0.01):
    xv, yv = matrix(val_rows)
    if len(yv) < 200 or len(set(yv.tolist())) < 2:
        return False, {"reason": "insufficient_validation_data", "n": int(len(yv))}
    pc = predict_blob(candidate, xv)
    pi = predict_blob(incumbent, xv) if incumbent else pc
    auc_c, br_c = roc_auc_score(yv, pc), brier_score_loss(yv, pc)
    auc_i, br_i = (roc_auc_score(yv, pi), brier_score_loss(yv, pi)) if incumbent else (auc_c, br_c)
    cand_pred_bler = float(1.0 - pc.mean())
    rep = {
        "auc_candidate": auc_c, "auc_incumbent": auc_i,
        "brier_candidate": br_c, "brier_incumbent": br_i,
        "candidate_pred_bler": cand_pred_bler, "floor_bler": floor_bler, "n_val": int(len(yv)),
    }
    ok = (auc_c >= auc_i - slack_auc) and (br_c <= br_i + slack_brier) and (cand_pred_bler <= floor_bler)
    rep["pass"] = bool(ok)
    return bool(ok), rep


def write_atomic(path, blob):
    tmp = path + ".tmp"
    with open(tmp, "w") as fh:
        json.dump(blob, fh)
    os.replace(tmp, path)


def log(msg):
    print(f"[{time.strftime('%Y-%m-%d %H:%M:%S')}] {msg}", flush=True)


def run_cycle(tp, model_out, data_dir, revert_flag, state):
    rows = load_newtx(data_dir)
    if len(rows) < tp["min_rows"]:
        log(f"have {len(rows)} rows, need {tp['min_rows']}; holding.")
        return state
    if len(rows) == state.get("last_n", 0):
        log(f"no new rows ({len(rows)}); skipping.")
        return state

    model, auc, brier, ntr, nte = T.train(
        rows, tp["n_estimators"], tp["max_depth"], tp["learning_rate"], tp["test_frac"]
    )
    if ntr < 1000 or nte < 200:
        log(f"insufficient post-dedup data (train={ntr} test={nte}); skipping.")
        return state
    meta = {"sources": "online", "rows": int(len(rows)), "auc": float(auc), "brier": float(brier)}
    lr, init, off, feat, thr, left, right, val = T.model_arrays(model)
    candidate = {
        "format": "ocudu_mcs_ml", "version": 1, "num_features": len(FEATURES), "features": FEATURES,
        "num_trees": len(model.estimators_), "learning_rate": lr, "init_logodds": init,
        "tree_offset": off, "node_feature": feat, "node_threshold": thr,
        "node_left": left, "node_right": right, "node_value": val, "meta": meta,
    }
    log(f"candidate trained: AUC={auc:.4f} Brier={brier:.4f} train={ntr} test={nte}")

    incumbent = None
    if os.path.exists(model_out):
        try:
            incumbent = json.load(open(model_out))
        except Exception:
            incumbent = None

    val_rows = rows[-tp["val_window"]:]
    passed, rep = validate(candidate, incumbent, val_rows, tp["floor_bler"])
    log(f"gate: {'PASS' if passed else 'REJECT'} {json.dumps(rep)}")

    if passed:
        if incumbent is not None:
            shutil.copyfile(model_out, model_out + ".lastgood")
        write_atomic(model_out, candidate)
        state["strikes"] = 0
        log(f"PROMOTED -> {model_out}")
    else:
        xv, yv = matrix(val_rows)
        live_bler = float(1.0 - yv.mean()) if len(yv) else 1.0
        if live_bler > tp["floor_bler"]:
            state["strikes"] = state.get("strikes", 0) + 1
            log(f"live BLER {live_bler:.3f} > floor {tp['floor_bler']:.3f}; strike {state['strikes']}/{tp['revert_strikes']}")
            if revert_flag and state["strikes"] >= tp["revert_strikes"]:
                open(revert_flag, "w").write("revert: online learning below floor\n")
                log(f"AUTO-REVERT: wrote {revert_flag}; learning frozen.")
        else:
            state["strikes"] = 0
            log("rejected but live model above floor; keeping current model.")

    state["last_n"] = len(rows)
    return state


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--config", required=True, help="DU YAML config (reads the ml_mcs block)")
    ap.add_argument("--once", action="store_true")
    args = ap.parse_args()

    cfg = load_cfg(args.config)
    tp = cfg["online_training"]
    dl = cfg["dataset_logging"]
    inf = cfg["inference"]

    if not tp["enabled"]:
        log("ml_mcs.online_training.enabled is false; nothing to do.")
        return

    data_dir = dl["output_dir"]
    model_out = inf["model_path"]
    if not model_out:
        sys.exit("ml_mcs.inference.model_path must be set for online training to promote models.")
    revert_flag = tp.get("revert_flag", "")

    log(f"online_trainer: interval={tp['interval_min']}min data_dir={data_dir} model_out={model_out} "
        f"min_rows={tp['min_rows']} val_window={tp['val_window']} floor_bler={tp['floor_bler']}")
    state = {"strikes": 0, "last_n": 0}
    while True:
        try:
            state = run_cycle(tp, model_out, data_dir, revert_flag, state)
        except Exception as e:
            log(f"cycle error (continuing): {type(e).__name__}: {e}")
        if args.once:
            break
        time.sleep(tp["interval_min"] * 60.0)


if __name__ == "__main__":
    main()
