// SPDX-FileCopyrightText: Copyright (C) 2021-2026 Software Radio Systems Limited
// SPDX-License-Identifier: BSD-3-Clause-Open-MPI
// Portions of this file may implement 3GPP specifications, which may be subject to additional licensing requirements.

#pragma once

#include <chrono>
#include <optional>

namespace ocudu {
namespace ocudu_ntn {

/// Structure used to send NTN Doppler Compensation Request to RU.
struct doppler_compensation_request {
  using time_point = std::chrono::system_clock::time_point;
  /// Sector Id to which the request applies.
  unsigned sector_id = 0;
  /// Carrier Frequency Offset in Hz.
  float cfo_hz = 0.0;
  /// Carrier Frequency Offset drift in Hz/s relative to the start time. Set it to 0 for no drift.
  float cfo_drift_hz_s = 0.0;
  /// Optional timestamp at which the CFO command is applied. Set to \c std::nullopt for applying it immediately.
  std::optional<time_point> start_timestamp;
};

/// Structure used to drive an emulated NTN channel from the propagated satellite geometry.
///
/// Only a simulated radio consumes this. It exists so the channel a UE flies through is derived from the same
/// ephemeris the gNB broadcasts in SIB19, rather than from an orbit model private to the simulator.
struct ntn_channel_emulation_request {
  /// Sector Id to which the request applies.
  unsigned sector_id = 0;
  // \brief Delay applied to the receive path of the emulated channel: the uplink leg.
  std::chrono::duration<double, std::micro> rx_delay{0};
  /// Rate of change of that delay, in microseconds per second. The radio turns this into a Doppler shift using
  /// its own centre frequencies, so the NTN layer does not need to know the carriers.
  double delay_drift_us_per_s = 0.0;
  // \brief Rate of change of the SERVICE leg alone, in microseconds per second, for the Doppler shift.
  double service_drift_us_per_s = 0.0;
  /// Whether the radio should also rotate the carrier by the service-link Doppler. Only correct when the UE
  /// compensates it too - see du_high_unit_ntn_serving_cell_config.
  bool emulate_doppler = false;
  // \brief Whether the satellite is above the horizon at the reference location.
  bool link_up = true;
};

/// \brief Interface for handling NTN Doppler compensation requests.
///
/// This interface decouples the NTN module from direct RU controller dependencies.
class ntn_doppler_compensation_handler
{
public:
  virtual ~ntn_doppler_compensation_handler() = default;

  /// \brief Handle DL Doppler compensation request.
  ///
  /// \param request Pre-calculated DL Doppler compensation request containing time, frequency, and drift values.
  /// \return True if the request was successfully handled; false otherwise.
  virtual bool handle_dl_doppler_compensation(const doppler_compensation_request& request) = 0;

  /// \brief Handle UL Doppler compensation request.
  ///
  /// \param request Pre-calculated UL Doppler compensation request containing time, frequency, and drift values.
  /// \return True if the request was successfully handled; false otherwise.
  virtual bool handle_ul_doppler_compensation(const doppler_compensation_request& request) = 0;

  // \brief Handle an NTN channel emulation request.
  virtual bool handle_ntn_channel_emulation(const ntn_channel_emulation_request& request) { return false; }
};

} // namespace ocudu_ntn
} // namespace ocudu
