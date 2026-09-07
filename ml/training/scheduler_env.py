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

import numpy as np
import torch

PRB_BANDWIDTH_HZ = {0: 180e3, 1: 360e3, 2: 720e3}
BLOCKLENGTH = {0: 24, 1: 48, 2: 96}

CELL_RADIUS_M = 500.0
PATHLOSS_REF_DB = 128.1
PATHLOSS_EXP = 3.76
SHADOWING_STD_DB = 10.0
NOISE_DBM = -110.0
P_MAX_W = 1.0
B_MAX_HZ = 720e3
N_LARGEST_PRBS = 12
XI_SET = (0.25, 0.5, 0.75)
GAMMA_THR_DB = 5.0
EPS_B = 1e-6
URLLC_PACKET_BYTES = 32
DT_MINI_S = 143e-6
DT_SHORT_S = 1e-3

ZONE_SET = [
    (0, 12, 0), (6, 0, 6), (4, 4, 4), (8, 4, 0), (8, 0, 4), (4, 0, 8),
    (4, 8, 0), (0, 8, 4), (0, 4, 8), (8, 2, 2), (2, 8, 2), (2, 2, 8),
]

NOISE_W = 10.0 ** ((NOISE_DBM - 30.0) / 10.0)
GAMMA_THR = 10.0 ** (GAMMA_THR_DB / 10.0)
QINV_EPS_B = 4.753424308822899

PRB_GROUP = 1


def set_prb_group(g):
    global PRB_GROUP
    PRB_GROUP = max(1, int(g))


def set_cell_bandwidth_mhz(bw_mhz):
    global N_LARGEST_PRBS
    N_LARGEST_PRBS = max(1, int(round(bw_mhz * 1e6 / B_MAX_HZ)))


def channel_gains(rng, n_users, n_prbs):
    d_km = np.sqrt(rng.uniform(0.0, 1.0, n_users)) * (CELL_RADIUS_M / 1000.0)
    d_km = np.maximum(d_km, 0.01)
    pl_db = PATHLOSS_REF_DB + 10.0 * PATHLOSS_EXP * np.log10(d_km)
    sh_db = rng.normal(0.0, SHADOWING_STD_DB, n_users)
    large = 10.0 ** (-(pl_db + sh_db) / 10.0)
    rayleigh = rng.exponential(1.0, (n_users, n_prbs))
    return large[:, None] * rayleigh


def draw_config(rng):
    while True:
        i_e, i_u = rng.integers(0, 3), rng.integers(0, 3)
        if i_e <= i_u:
            break
    zones = ZONE_SET[rng.integers(0, len(ZONE_SET))]
    if N_LARGEST_PRBS != 12:
        scale = N_LARGEST_PRBS / 12.0
        n_e, n_sh = int(round(zones[0] * scale)), int(round(zones[1] * scale))
        zones = (n_e, n_sh, N_LARGEST_PRBS - n_e - n_sh)
    xi = XI_SET[rng.integers(0, len(XI_SET))]
    return int(i_e), int(i_u), zones, float(xi)


def urllc_sample(rng, n_urllc_users=4, arrival_pkt_per_ms=1.5):
    i_e, i_u, (n_e, n_sh, n_u), xi = draw_config(rng)
    bw_urllc = (n_u + n_sh) * B_MAX_HZ
    n_prbs = int(bw_urllc // PRB_BANDWIDTH_HZ[i_u])
    if n_prbs == 0:
        return urllc_sample(rng, n_urllc_users, arrival_pkt_per_ms)

    n_units = max(1, n_prbs // PRB_GROUP)
    gains   = channel_gains(rng, n_urllc_users, n_units)
    pkts = rng.poisson(arrival_pkt_per_ms * DT_MINI_S * 1e3, n_urllc_users)
    demand = pkts.astype(np.float64) * URLLC_PACKET_BYTES * 8.0

    n_units_urllc_only = int(n_u * B_MAX_HZ // PRB_BANDWIDTH_HZ[i_u]) // PRB_GROUP
    punct  = np.zeros(n_units)
    shared = np.arange(n_units) >= n_units_urllc_only
    punct[shared] = (rng.uniform(0.0, 1.0, int(shared.sum())) < 0.5).astype(np.float64)

    return {
        "gains": gains,
        "demand": demand,
        "punct": punct,
        "power_budget": (1.0 - xi) * P_MAX_W,
        "blocklength": BLOCKLENGTH[i_u] * PRB_GROUP,
        "prb_bw_hz": PRB_BANDWIDTH_HZ[i_u] * PRB_GROUP,
        "numerology": i_u,
        "prb_group": PRB_GROUP,
    }


def embb_sample(rng, n_embb_users=6):
    i_e, i_u, (n_e, n_sh, n_u), xi = draw_config(rng)
    bw_embb = (n_e + n_sh) * B_MAX_HZ
    n_prbs = int(bw_embb // PRB_BANDWIDTH_HZ[i_e])
    if n_prbs == 0:
        return embb_sample(rng, n_embb_users)

    n_units = max(1, n_prbs // PRB_GROUP)
    gains   = channel_gains(rng, n_embb_users, n_units)
    n_units_embb_only = int(n_e * B_MAX_HZ // PRB_BANDWIDTH_HZ[i_e]) // PRB_GROUP
    shared  = (np.arange(n_units) >= n_units_embb_only).astype(np.float64)
    slot_dur_s = {0: 1e-3, 1: 0.5e-3, 2: 0.25e-3}[i_e]

    return {
        "gains": gains,
        "shared": shared,
        "power_budget": xi * P_MAX_W,
        "prb_bw_hz": PRB_BANDWIDTH_HZ[i_e] * PRB_GROUP,
        "n_tti": max(1, int(DT_SHORT_S / slot_dur_s)),
        "numerology": i_e,
        "prb_group": PRB_GROUP,
    }


_CHANNEL_DISPERSION = 1.0 - 1.0 / (1.0 + GAMMA_THR) ** 2
_SHANNON_PER_SYMBOL = np.log2(1.0 + GAMMA_THR)
_PENALTY_COEF = np.log2(np.e) * QINV_EPS_B * np.sqrt(_CHANNEL_DISPERSION)


def urllc_rate_bits(n_alloc_prbs, blocklength):
    x = np.asarray(blocklength, dtype=np.float64) * np.asarray(n_alloc_prbs, dtype=np.float64)
    return np.maximum(0.0, x * _SHANNON_PER_SYMBOL - _PENALTY_COEF * np.sqrt(x))


def urllc_rate_bits_torch(n_alloc_prbs, blocklength):
    x = blocklength * n_alloc_prbs
    return torch.clamp(x * _SHANNON_PER_SYMBOL - _PENALTY_COEF * torch.sqrt(x), min=0.0)


def urllc_pair_power(gain):
    return GAMMA_THR * NOISE_W / gain
