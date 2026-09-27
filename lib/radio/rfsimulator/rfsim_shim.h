// SPDX-FileCopyrightText: Copyright (C) 2021-2026 Software Radio Systems Limited
// SPDX-License-Identifier: BSD-3-Clause-Open-MPI

#pragma once

namespace ocudu {

void rfsim_config_set(const char* name, const char* value);

void rfsim_set_log_sink(void (*sink)(int level, const char* message));

void rfsim_set_log_level(int level);

// \brief Sets the geometry of the emulated NTN channel applied to received samples.
void rfsim_set_ntn_channel(double rx_delay_us, double drift_us_per_s, bool link_up);

// \brief Sets how much of the round trip the downlink carries, 0 to 1.
void rfsim_set_ntn_dl_share(double share);

/// Sets the Doppler shift applied to the received carrier, in Hertz.
void rfsim_set_ntn_doppler(double doppler_hz);

}
