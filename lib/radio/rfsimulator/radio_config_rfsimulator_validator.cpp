// SPDX-FileCopyrightText: Copyright (C) 2021-2026 Software Radio Systems Limited
// SPDX-License-Identifier: BSD-3-Clause-Open-MPI

#include "radio_config_rfsimulator_validator.h"
#include "fmt/base.h"
#include <cmath>

using namespace ocudu;

static bool validate_stream(const radio_configuration::stream& stream)
{
  if (stream.channels.empty()) {
    fmt::print("Streams must contain at least one channel.\n");
    return false;
  }

  for (const radio_configuration::channel& channel : stream.channels) {
    if (!channel.args.empty()) {
      fmt::print("Per-channel arguments are not supported by the rfsimulator radio. Use device_args.\n");
      return false;
    }
  }

  if (!stream.args.empty()) {
    fmt::print("Stream arguments are not supported by the rfsimulator radio. Use device_args.\n");
    return false;
  }

  return true;
}

bool radio_config_rfsimulator_validator::is_configuration_valid(const radio_configuration::radio& config) const
{
  if (config.clock.clock != radio_configuration::clock_sources::source::DEFAULT ||
      config.clock.sync != radio_configuration::clock_sources::source::DEFAULT) {
    fmt::print("Only 'default' clock and sync sources are available with the rfsimulator radio.\n");
    return false;
  }

  if ((config.tx_streams.size() != 1) || (config.rx_streams.size() != 1)) {
    fmt::print("The rfsimulator radio supports exactly one transmit and one receive stream.\n");
    return false;
  }

  if (!validate_stream(config.tx_streams[0]) || !validate_stream(config.rx_streams[0])) {
    return false;
  }

  if (!std::isnormal(config.sampling_rate_Hz) || (config.sampling_rate_Hz < 0.0)) {
    fmt::print("The sampling rate must be positive, non-zero, NAN nor infinite.\n");
    return false;
  }

  if (config.otw_format != radio_configuration::over_the_wire_format::DEFAULT &&
      config.otw_format != radio_configuration::over_the_wire_format::SC16) {
    fmt::print("The rfsimulator radio transports 16-bit complex samples only.\n");
    return false;
  }

  if (config.tx_mode != radio_configuration::transmission_mode::continuous) {
    fmt::print("Discontinuous transmission modes are not supported by the rfsimulator radio.\n");
    return false;
  }

  return true;
}
