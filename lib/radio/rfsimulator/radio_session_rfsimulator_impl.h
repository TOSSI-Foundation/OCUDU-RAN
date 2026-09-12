// SPDX-FileCopyrightText: Copyright (C) 2021-2026 Software Radio Systems Limited
// SPDX-License-Identifier: BSD-3-Clause-Open-MPI

#pragma once

#include "common_lib.h"
#include "ocudu/adt/complex.h"
#include "ocudu/adt/span.h"
#include "ocudu/gateways/baseband/baseband_gateway_receiver.h"
#include "ocudu/gateways/baseband/baseband_gateway_transmitter.h"
#include "ocudu/radio/radio_session.h"
#include "ocudu/support/executors/task_executor.h"
#include <atomic>
#include <chrono>
#include <vector>

namespace ocudu {

class radio_session_rfsimulator_impl : public radio_session,
                                       public radio_management_plane,
                                       public baseband_gateway,
                                       public baseband_gateway_transmitter,
                                       public baseband_gateway_receiver
{
public:
  radio_session_rfsimulator_impl() = delete;

  radio_session_rfsimulator_impl(const radio_configuration::radio& config,
                                 task_executor&                    async_task_executor,
                                 radio_event_notifier&             notification_handler);

  ~radio_session_rfsimulator_impl() override;

  bool is_successful() const { return successful; }

  radio_management_plane& get_management_plane() override { return *this; }

  baseband_gateway& get_baseband_gateway(unsigned stream_id) override
  {
    ocudu_assert(stream_id == 0, "Only a single stream is supported, requested {}.", stream_id);
    return *this;
  }

  baseband_gateway_timestamp read_current_time() override { return 0; }

  void start(baseband_gateway_timestamp init_time) override;

  void stop() override;

  bool set_tx_gain(unsigned port_id, double gain_dB) override { return true; }
  bool set_rx_gain(unsigned port_id, double gain_dB) override { return true; }
  bool set_tx_freq(unsigned stream_id, double center_freq_Hz) override { return true; }
  bool set_rx_freq(unsigned stream_id, double center_freq_Hz) override { return true; }

  bool set_ntn_channel(double one_way_delay_us,
                       double delay_drift_us_per_s,
                       double service_drift_us_per_s,
                       bool   emulate_doppler,
                       bool   link_up) override;

  baseband_gateway_transmitter& get_transmitter() override { return *this; }
  baseband_gateway_receiver&    get_receiver() override { return *this; }

  metadata receive(baseband_gateway_buffer_writer& data) override;

  void transmit(const baseband_gateway_buffer_reader& data, const baseband_gateway_transmitter_metadata& md) override;

private:
  ocudulog::basic_logger& logger;

  openair0_device_t device;
  openair0_config_t oai_config;

  baseband_gateway_timestamp ts_offset = 0;

  bool client_role = false;
  bool     channel_model_enabled = false;
  bool     rx_trace_enabled      = false;
  uint64_t rx_trace_count        = 0;
  double   rx_trace_peak         = 0.0;
  double   rx_trace_rms          = 0.0;
  unsigned rx_trace_short        = 0;

  void apply_tx_doppler(span<ci16_t> samples);

  std::atomic<double>              tx_doppler_hz{0.0};
  std::atomic<bool>                ntn_doppler_enabled{false};
  std::atomic<bool>                ntn_link_up{true};
  double                           tx_doppler_phase = 0.0;
  std::vector<std::vector<ci16_t>> tx_scratch;

  void pace_to_timestamp(baseband_gateway_timestamp ts);

  bool                                  pace_enabled         = false;
  double                                pace_sample_period_s = 0.0;
  bool                                  pace_origin_valid    = false;
  std::chrono::steady_clock::time_point pace_origin_tp;
  baseband_gateway_timestamp            pace_origin_ts   = 0;
  uint64_t                              pace_total_count = 0;
  uint64_t                              pace_late_count  = 0;
  std::chrono::nanoseconds              pace_max_slip{0};

  std::atomic<bool> stopped    = {false};
  bool              successful = false;
  bool              started    = false;
};

}
