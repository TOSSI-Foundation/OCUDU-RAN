"""Shared constants and helpers for the BSR-periodicity ML pipeline."""

TRIGGER_TYPE_UNKNOWN = 0
TRIGGER_TYPE_REGULAR = 1
TRIGGER_TYPE_PERIODIC = 2
TRIGGER_TYPE_PADDING = 3

VALID_PERIODIC_BSR_TIMER_SUBFRAMES = [1, 5, 10, 16, 20, 32, 40, 64, 80, 128, 160, 320, 640, 1280, 2560]

REQUIRED_COLUMNS = [
    "rnti",
    "ue_index",
    "slot",
    "numerology",
    "bsr_trigger_type",
    "has_interarrival",
    "interarrival_slots",
]


def map_to_nearest_periodicity(predicted_subframes):
    """Nearest valid periodicBSR-Timer (TS 38.331) to a predicted subframe count."""
    return min(VALID_PERIODIC_BSR_TIMER_SUBFRAMES, key=lambda eta: abs(eta - predicted_subframes))


def slots_to_subframes(slots, numerology):
    """Convert a slot count to nof. subframes (ms), given SCS numerology (2**numerology slots per subframe)."""
    return slots / float(1 << numerology)
