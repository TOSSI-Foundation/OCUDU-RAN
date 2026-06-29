# ML-based UL MCS Link Adaptation

Machine-learning uplink MCS selection for the OCUDU scheduler. A gradient-boosted model predicts
`P(crc_success | channel state, candidate MCS)`. At decision time the scheduler sweeps candidate MCS
and picks the highest whose predicted success meets the configured BLER target, falling back to OLLA
otherwise.

The deployed model is a self-contained C++ tree traversal; no ML runtime is linked into the RAN.
All behaviour is driven by the `ml_mcs` block of the DU YAML and is disabled by default.

---

## Implementation

### Decision model

- Target: `P(crc_success)` for a given `(channel state, candidate MCS)`, a calibrated probability, not
  a hard class. This is what makes the BLER target a tunable knob.
- Features (fixed order): `mcs`, `mcs_table`, `wideband_cqi`, `pusch_avg_sinr_db`, `ul_snr_offset_db`.
- Label: `crc_success` (CRC pass/fail), trained on newTx rows only.
- Algorithm: scikit-learn `GradientBoostingClassifier`, exported to flat C++ arrays plus a ~10-line
  traversal that reproduces `predict_proba` exactly. Two artifacts are emitted:
  - `mcs_ml_model.inc`: compiled-in seed (fallback if no runtime model is configured).
  - `<model>.model`: JSON runtime model loaded at startup and hot-swapped without a rebuild.

### Decision rule (in the scheduler)

In `calculate_ul_mcs()` the predictor sweeps MCS from high to low and returns the highest MCS with
`P(success) >= 1 - bler_target`. If none qualifies it returns the OLLA value. Inference runs only when
`ml_mcs.inference.enabled` is set; otherwise the scheduler behaves exactly as stock OLLA.

### Dataset generation

At CRC indication time (`ue_cell::handle_crc_pdu`) the logger writes one row per decoded PUSCH: the
channel state and grant parameters (features) joined atomically with the CRC outcome (label), plus the
model's predicted success for the MCS that flew. Output is a timestamped CSV in
`ml_mcs.dataset_logging.output_dir`, tagged with `ml_mcs.dataset_logging.scenario`.

### Online training (passive, no exploration)

A sidecar process retrains on the full accumulated data on a fixed interval, validates the candidate,
promotes it, and the running DU hot-swaps it. The model only ever selects its greedy best MCS; it
never transmits a deliberately-suboptimal one, so it cannot harm the link. It adapts to the
deployment's drift from observed outcomes alone.

Promotion gate (a candidate is promoted only if all hold):
- AUC does not regress versus the live model (small slack),
- calibration (Brier) does not regress,
- predicted BLER on the recent validation window stays under `floor_bler`.

If the live model stays below the safety floor for two consecutive cycles, the trainer touches
`revert_flag`; the predictor detects it and falls back to OLLA on every slot until cleared. Model
hot-swap and the revert check are throttled and run off the scheduling hot path.

### Components

| Path | Role |
|---|---|
| `../lib/scheduler/support/mcs_ml_predictor.h` | Inference, runtime model load, hot-swap, auto-revert. |
| `../lib/scheduler/support/mcs_ml_model.inc` | Compiled seed model. |
| `../lib/scheduler/logging/ml_la_dataset_logger.{h,cpp}` | CRC-time dataset logger. |
| `ml_config.py` | Shared reader for the `ml_mcs` block of the DU YAML. |
| `training/train_export_mcs.py` | Train a model; export `.inc` + `.model`. |
| `serving/online_trainer.py` | Periodic retrain + validation gate + auto-revert. |
| `analysis/analyse.py` | OLLA-vs-ML SINR-matched throughput and per-MCS coverage. |
| `models/` | Runtime model artifacts (seed shipped here). |
| `datasets/` | Dataset CSVs produced by the in-RAN logger. |

The shipped seed (`models/mcs_ml_seed.model`) was trained on **900k+ newTx rows of real over-the-air
(OTA) data** collected from a live deployment. It is a warm-start: usable out of the box, then adapted
to a new deployment by online training or by retraining on local/other data.

---

## Configuration

All parameters live in the DU YAML `ml_mcs` block; the C++ scheduler and the Python sidecar read the
same file. See `../configs/ml_mcs_example.yaml`.

```yaml
ml_mcs:
  inference:
    enabled: false                          # ML controls UL MCS (else OLLA)
    bler_target: 0.10                       # decision-rule target
    model_path: ml/models/mcs_ml_seed.model # runtime model; empty -> compiled seed
  dataset_logging:
    enabled: false                          # write one CSV row per decoded PUSCH
    output_dir: ml/datasets
    scenario: default                       # tag written into each row
  online_training:
    enabled: false                          # run the sidecar trainer loop
    interval_min: 15
    min_rows: 20000                         # rows required before the first retrain
    val_window: 10000                       # rows used for the validation gate
    floor_bler: 0.30                        # safety floor for auto-revert
    revert_flag: /tmp/ocudu_ml_revert       # trainer touches this to force OLLA fallback
```

Dependencies (Python): `numpy`, `scikit-learn`, `pyyaml`.

---

## Deployment and usage

### 1. Collect a dataset

Enable logging in the DU YAML, then run the DU. CSVs land in `output_dir`.

```yaml
ml_mcs:
  dataset_logging:
    enabled: true
    output_dir: ml/datasets
    scenario: site_a
```

### 2. Train a model

```
python3 ml/training/train_export_mcs.py ml/datasets/ul_mcs_dataset_*.csv \
    --out lib/scheduler/support/mcs_ml_model.inc \
    --model-out ml/models/site_a.model
```

This prints the held-out AUC/Brier and writes both the compiled seed (`.inc`) and the runtime model
(`.model`). Rebuild the DU only if you changed the compiled `.inc`; the `.model` is loaded at runtime.

#### Using an external or third-party dataset

If you cannot collect data on your own deployment (no UE, no test time, or a new environment), you can
train on any dataset from another source (another OCUDU deployment, a lab capture, a simulator, or a
public RAN dataset) as long as it provides the required columns:

`mcs, mcs_table, wideband_cqi, pusch_avg_sinr_db, ul_snr_offset_db, nof_retxs, crc_success`

Place such CSVs in `ml/datasets/` (or pass their paths directly to the trainer) and train as above. You
can also skip training entirely and run the shipped seed model; it works out of the box and online
training will adapt it to your environment over time.

Caveat: a model trained on a different environment is a starting point, not a tuned model for yours.
Its behaviour should be validated with `analysis/analyse.py`, and online training (or a retrain on local
data once collected) is recommended to adapt it.

### 3. Enable inference

```yaml
ml_mcs:
  inference:
    enabled: true
    bler_target: 0.10
    model_path: ml/models/site_a.model
```

Run the DU. ML now selects UL MCS; with `OCUDU_ML_MCS_LOG`-style decision logging the scheduler emits
`ML_LA:` lines (greppable in the DU log) showing the model vs OLLA pick per decision.

### 4. Enable online learning

```yaml
ml_mcs:
  online_training:
    enabled: true
    interval_min: 15
    min_rows: 20000
    revert_flag: /tmp/ocudu_ml_revert
```

Start the sidecar alongside the running DU, pointed at the same DU YAML:

```
python3 ml/serving/online_trainer.py --config configs/<du_config>.yaml
```

It retrains every `interval_min`, validates against the floor, atomically writes the `.model`, and the
DU hot-swaps it. On `--once` it runs a single cycle (useful for testing).

### 5. Compare against OLLA

```
python3 ml/analysis/analyse.py --data-dir ml/datasets
```

Reports OLLA-vs-ML effective throughput (delivered bytes per transmission) SINR-matched, plus per-MCS
coverage and BLER.

---

## Operational notes

- Start with `inference.enabled: false` to confirm the build behaves as stock OLLA, then enable ML.
- Effective throughput (delivered bytes/tx), not raw BLER, is the metric that decides whether ML helps:
  the model trades BLER for higher MCS where it predicts success.
- A seed model trained on one environment may be mis-tuned for another; online training adapts it to
  the local deployment over successive cycles.
- The auto-revert flag is the live kill-switch: deleting or recreating it is safe at any time.
