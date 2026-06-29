# ML-based BSR Periodicity Prediction

The `periodicBSR-Timer` (TS 38.331 `BSR-Config`) is a single static value applied to every UE. A short
timer keeps the gNB's view of the UE buffer fresh but costs signalling; a long timer is cheap but the
gNB learns about new uplink data late.

This feature predicts each UE's next uplink data interarrival time from its recent history, maps the
prediction to the nearest legal `periodicBSR-Timer`, and reconfigures that UE — per UE, at runtime.

---

## Implementation

### Model

Support Vector Regression over Random Fourier Features (SVR-RFF):

```
StandardScaler -> RBFSampler(gamma=1, n_components=100) -> SVR(kernel="linear", C=100, epsilon=0.1)
```

Input is a sliding window of the last **30** interarrival times (in slots) for one UE. Output is the
predicted next interarrival, also in slots. Inference in C++ is a direct evaluation of the fitted
arrays — no runtime ML dependency:

```
x'  = (x - scaler_mean) / scaler_scale
Z_j = sqrt(2/n) * cos(x' . W_j + b_j)
y   = sum_j coef_j * Z_j + intercept
```

### Mapping to the standard

The predicted slot count is converted to subframes (`y / 2^numerology`) and snapped to the nearest
value of the `periodicBSR-Timer` collection (TS 38.331):

```
1, 5, 10, 16, 20, 32, 40, 64, 80, 128, 160, 320, 640, 1280, 2560   (subframes = ms)
```

`infinity` is excluded — it has no finite distance, so it cannot participate in the argmin.

### Training data

Only **regular** BSRs are used (TS 38.321 clause 5.4.5). Periodic and padding BSRs are timer- and
grant-driven, not new-data arrivals, so they carry no interarrival signal. The dataset logger classifies
every BSR by trigger type and records the interarrival only for regular ones.

### Damping gate

A raw prediction stream would thrash the UE with reconfigurations. Before any change is applied, three
checks run in order (`lib/du/du_high/du_manager/du_manager_impl.cpp`):

| # | Check | Effect |
|---|---|---|
| 1 | already-applied | skip if the recommendation equals the value in force |
| 2 | hysteresis | skip if the recommendation is within `1` index step of the value in force |
| 3 | min-dwell | skip if fewer than `10 s` have elapsed since the last reconfiguration |

Checks 2 and 3 are guarded on a previously applied value, so the **first** move away from the
configured static timer is always allowed. Both constants are compile-time
(`BSR_ACT_HYSTERESIS_STEPS`, `BSR_ACT_MIN_DWELL_SEC`); they are not exposed in the YAML.

### Actuation

Once the gate passes, the DU rewrites `bsr_cfg.periodic_timer`, packs a `CellGroupConfig` diff, and
sends **F1AP UEContextModificationRequired** (TS 38.473 clause 8.3.5) with
`DUtoCURRCInformation.cellGroupConfig` and `cause = action_desirable_for_radio_reasons`. The CU-CP
forwards it to the UE as an RRC Reconfiguration, and the UE applies the new timer.

The DU sends and returns; it does not wait for `UEContextModificationConfirm` / `Refuse`.

### Components

| Path | Role |
|---|---|
| `../lib/scheduler/support/bsr_periodicity_predictor.h` | Inference, runtime model load, hot-swap. |
| `../lib/scheduler/support/bsr_periodicity_model.inc` | Compiled seed model. |
| `../lib/scheduler/logging/bsr_ml_dataset_logger.{h,cpp}` | BSR trigger classifier + dataset logger. |
| `../lib/du/du_high/du_manager/procedures/du_bsr_periodicity_actuation_procedure.{h,cpp}` | F1AP actuation. |
| `bsr_ml_config.py` | Shared reader for the `bsr_ml` block and the timer collection. |
| `training/train_bsr_periodicity.py` | Train a model; export `.inc` + `.model`. |
| `analysis/analyze_bsr_run.py` | Per-run BSR counts, overhead, latency estimate. |
| `analysis/compare_runs.py` | Side-by-side comparison of runs. |
| `models/` | Runtime model artifacts (seed shipped here). |
| `datasets/` | Dataset CSVs produced by the in-RAN logger. |

The shipped seed was trained on **60,386 sliding windows** drawn from **15 real captures**. It is
compiled into the DU binary via the `.inc`, so inference works out of the box with no external model
file. The `.model` is an optional hot-swappable override, reloaded on mtime change without a restart.

`bsr_periodicity_model.inc` and `bsr_periodicity.model` must describe the same model — the first is the
compiled fallback, the second the runtime override. Export both from the same training run. Every
`.model` records the captures it came from:

```bash
python3 -c "import json;m=json.load(open('ml/models/bsr_periodicity.model'))['meta']; \
            print(len(m['sources']),'captures |',m['windowed_samples'],'windows')"
```

Retraining overwrites both artifacts. Back them up first: the `.inc` is untracked, so git cannot
restore it.

---

## Configuration

All parameters live in the DU YAML `bsr_ml` block. See [`../configs/bsr_ml_example.yaml`](../configs/bsr_ml_example.yaml).

```yaml
bsr_ml:
  inference:
    enabled: false                              # predict a periodicBSR-Timer per UE
    model_path: ml/models/bsr_periodicity.model # runtime model; empty -> compiled seed
  actuation:
    enabled: false                              # apply the prediction via F1AP; needs inference
  dataset_logging:
    enabled: false                              # write one CSV row per BSR
    output_dir: ml/datasets
    scenario: default                           # tag written into each row
```

The static baseline the ML moves away from is a normal MAC setting, not part of the `bsr_ml` block:

```yaml
cell_cfg:
  mac_cell_group:
    bsr_cfg:
      periodic_bsr_timer: 40                    # subframes; must be a legal TS 38.331 value
```

The three flags are independent layers and are meant to be enabled in order:

| Mode | Flags | Behaviour |
|---|---|---|
| Collect | `dataset_logging` | Writes the CSV. No prediction, no change. |
| Shadow | `+ inference` | Predicts and reports per UE. Nothing is applied. |
| Closed loop | `+ actuation` | Reconfigures the UE over F1AP. |

`actuation` without `inference` is a no-op. Paths are relative to the working directory.

Logging must be at `info` or the actuation lines are not printed:

```yaml
log:
  all_level: info
  f1ap_json_enabled: true   # optional: dumps the F1AP PDU
```

Dependencies (Python): `numpy`, `scikit-learn`, `pyyaml`, `matplotlib` (plots only).

---

## Deployment and usage

### 1. Collect a dataset

Run with a static timer and logging on. Prediction and actuation stay off.

```bash
sudo ./build/apps/du/odu -c configs/demo_sf10.yaml
```

Confirm on startup:

```
[bsr_ml_dataset] BSR dataset logging enabled -> ml/datasets/bsr_ml_dataset_<ts>.csv (scenario=sf10, periodicBSR-Timer=10 sf, ...)
```

Check the capture before using it. The `regular` count is what matters — nothing else trains:

```bash
python3 ml/analysis/analyze_bsr_run.py ml/datasets/bsr_ml_dataset_<ts>.csv
```

A window is 30 samples, so a UE needs **more than 30 regular BSRs** before it yields even one training
row, and `--min-samples` defaults to 200. Short or idle captures are silently useless.

### 2. Train a model

```bash
cp lib/scheduler/support/bsr_periodicity_model.inc /tmp/seed.inc.bak   # untracked; no git restore

python3 ml/training/train_bsr_periodicity.py ml/datasets/bsr_ml_dataset_*.csv \
    --inc-out   lib/scheduler/support/bsr_periodicity_model.inc \
    --model-out ml/models/bsr_periodicity.model
```

Training prints held-out error and a mapping demo. Check that predictions span **more than one** timer
bucket; a model that snaps everything to a single value has collapsed and should not be shipped.

Only captures taken with `inference: false` should be used. Training on a capture the model itself
influenced is a feedback loop.

Regenerating the `.inc` requires a rebuild to take effect:

```bash
cmake --build build -j$(nproc) --target odu
```

### 3. Enable inference (shadow mode)

```yaml
bsr_ml:
  inference: { enabled: true, model_path: ml/models/bsr_periodicity.model }
  actuation: { enabled: false }
```

The predicted timer appears per UE in the `predicted_periodicity_subframes` column of the dataset CSV.
Nothing is reconfigured. This is the safe way to evaluate a model on a live cell.

### 4. Enable actuation (closed loop)

```bash
sudo ./build/apps/du/odu -c configs/du_bsr_ml_e2e.yaml
```

Expected, in order:

```
ue=0: Actuating BSR periodicity change sf40 -> sf64 via F1AP UE Context Modification Required.
ue=0 du_ue_id=0: Sending UEContextModificationRequired (adaptive BSR periodicity).
```

Then the CU-CP emits an RRC Reconfiguration carrying the new `BSR-Config`.

### 5. Compare against a static timer

Capture one run per fixed timer, then:

```bash
python3 ml/analysis/compare_runs.py sf10=ml/datasets/run_sf10.csv \
                                    sf64=ml/datasets/run_sf64.csv \
                                    ML=ml/datasets/run_ML.csv
```

The comparison is only valid if the runs carried the **same traffic**. Check the traffic-match rows
(total MB, mean kbps, buffer-occupied %) first. Periodic BSRs only fire while data is pending, so a
lighter run reports fewer of them regardless of its timer.

The `reporting-delay bound` row is the applied timer itself: it is not confounded by traffic volume and
is comparable across runs. The `D1 Little's-law` row is sampled at BSR instants, so a longer timer
inflates it; treat it as indicative only.

---

## Operational notes

- **Warm-up.** No prediction is emitted for a UE until it has produced 30 regular BSRs. On an idle UE
  this never happens, and the feature is inert by design.
- **Reconfiguration rate.** Bounded to one change per UE per 10 s by min-dwell, and one-index-step
  moves are suppressed entirely.
- **Silent suppression.** The gate does not log when it drops a recommendation, so the absence of
  actuation lines does not imply the predictor is idle.
- **Hot reload.** Overwriting `model_path` swaps the model on the next mtime poll, with no restart. The
  compiled seed remains the fallback if the file is absent or fails to load.
- **First move.** Hysteresis and min-dwell only apply once a value has been applied by the ML, so the
  first transition away from the configured static timer is not damped.
- **Confirm/Refuse.** The DU does not consume `UEContextModificationConfirm` / `Refuse`; the
  reconfiguration is fire-and-forget from the DU's point of view.

---

## References

N. Chukhno, S. Saafi and S. Andreev, "ML-Aided Dynamic BSR Periodicity Adjustment for Enhanced UL
Scheduling in Cellular Systems," in *IEEE Open Journal of the Communications Society*, vol. 6,
pp. 3513-3527, 2025, doi: [10.1109/OJCOMS.2025.3561002](https://doi.org/10.1109/OJCOMS.2025.3561002).

Keywords: Dynamic scheduling; Predictive models; 3GPP; Scheduling; Quality of service;
Telecommunication traffic; Uplink; Prediction algorithms; Long short term memory; Ultra reliable low
latency communication; Buffer status report; uplink scheduling; cellular networks; machine learning;
uplink traffic prediction; wireless communications; machine learning for communications.

3GPP TS 38.321, *Medium Access Control (MAC) protocol specification* — clause 5.4.5 (BSR triggers).

3GPP TS 38.331, *Radio Resource Control (RRC) protocol specification* — `BSR-Config`.

3GPP TS 38.473, *F1 Application Protocol (F1AP)* — clause 8.3.5 (UE Context Modification Required).
