// SPDX-FileCopyrightText: Copyright (C) 2021-2026 Software Radio Systems Limited
// SPDX-License-Identifier: BSD-3-Clause-Open-MPI

#pragma once

namespace ocudu {

void rfsim_config_set(const char* name, const char* value);

void rfsim_set_log_sink(void (*sink)(int level, const char* message));

void rfsim_set_log_level(int level);

void rfsim_set_ntn_channel(double rx_delay_us, double drift_us_per_s, bool link_up);

void rfsim_set_ntn_doppler(double doppler_hz);
}
