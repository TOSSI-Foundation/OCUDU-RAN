import os
import sys

try:
    import yaml
except ImportError:
    yaml = None

FEATURES = ["mcs", "mcs_table", "wideband_cqi", "pusch_avg_sinr_db", "ul_snr_offset_db"]
LABEL = "crc_success"

DEFAULTS = {
    "inference": {
        "enabled": False,
        "bler_target": 0.10,
        "model_path": "",
    },
    "dataset_logging": {
        "enabled": False,
        "output_dir": "ml/datasets",
        "scenario": "default",
    },
    "online_training": {
        "enabled": False,
        "interval_min": 15,
        "min_rows": 20000,
        "val_window": 10000,
        "floor_bler": 0.30,
        "test_frac": 0.2,
        "revert_strikes": 2,
        "revert_flag": "",
        "n_estimators": 200,
        "max_depth": 4,
        "learning_rate": 0.08,
    },
}


def _merge(base, override):
    out = dict(base)
    for k, v in (override or {}).items():
        if isinstance(v, dict) and isinstance(out.get(k), dict):
            out[k] = _merge(out[k], v)
        else:
            out[k] = v
    return out


def load(config_path):
    if yaml is None:
        sys.exit("pyyaml is required: pip install pyyaml")
    if not os.path.isfile(config_path):
        sys.exit(f"config not found: {config_path}")
    with open(config_path) as fh:
        doc = yaml.safe_load(fh) or {}
    block = doc.get("ml_mcs", {})
    cfg = {section: _merge(DEFAULTS[section], block.get(section, {})) for section in DEFAULTS}
    return cfg


def section(config_path, name):
    return load(config_path)[name]
