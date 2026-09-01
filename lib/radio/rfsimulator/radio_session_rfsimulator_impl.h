// SPDX-FileCopyrightText: Copyright (C) 2021-2026 Software Radio Systems Limited
// SPDX-License-Identifier: BSD-3-Clause-Open-MPI

#pragma once

#include "ocudu/gateways/baseband/baseband_gateway_receiver.h"
#include "ocudu/gateways/baseband/baseband_gateway_transmitter.h"
#include "ocudu/radio/radio_session.h"
#include "ocudu/support/executors/task_executor.h"
#include <atomic>

#include "common_lib.h"

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

  std::atomic<bool> stopped    = {false};
  bool              successful = false;
  bool              started    = false;
};

}
