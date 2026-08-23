WINDOW_SIZE = 4

TARGET_COLUMN = "effective_cqi"

REQUIRED_COLUMNS = [
    "rnti",
    "ue_index",
    "slot",
    "numerology",
    "effective_cqi",
]

def is_usable_effective_cqi(value):
    try:
        return float(value) > 0.0
    except (TypeError, ValueError):
        return False
