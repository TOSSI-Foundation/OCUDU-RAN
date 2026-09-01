// SPDX-FileCopyrightText: Copyright (C) 2021-2026 Software Radio Systems Limited
// SPDX-License-Identifier: BSD-3-Clause-Open-MPI

#include "radio_session_rfsimulator_impl.h"
#include "rfsim_shim.h"
#include "ocudu/gateways/baseband/buffer/baseband_gateway_buffer_reader.h"
#include "ocudu/gateways/baseband/buffer/baseband_gateway_buffer_writer.h"
#include "ocudu/ocudulog/ocudulog.h"
#include "ocudu/ocuduvec/zero.h"

using namespace ocudu;

static_assert(sizeof(ci16_t) == sizeof(c16_t), "Baseband buffers must match the rfsimulator sample format");

static constexpr const char* DEFAULT_SERVER_PORT = "4043";

namespace {

void log_sink(int level, const char* message)
{
  static ocudulog::basic_logger& log = ocudulog::fetch_basic_logger("RF");

  std::string_view text(message);
  while (!text.empty() && (text.back() == '\n')) {
    text.remove_suffix(1);
  }

  switch (level) {
    case 0:
      log.error("rfsim: {}", text);
      break;
    case 1:
      log.warning("rfsim: {}", text);
      break;
    case 2:
      log.info("rfsim: {}", text);
      break;
    default:
      log.debug("rfsim: {}", text);
      break;
  }
}

void apply_device_args(const std::string& args, ocudulog::basic_logger& logger, bool& client_role)
{
  size_t pos = 0;
  while (pos < args.size()) {
    size_t end = args.find_first_of(", ", pos);
    if (end == std::string::npos) {
      end = args.size();
    }

    std::string_view item(args.data() + pos, end - pos);
    pos = end + 1;
    if (item.empty()) {
      continue;
    }

    size_t sep = item.find('=');
    if (sep == std::string_view::npos) {
      logger.warning("Ignoring rfsimulator argument '{}': expected name=value.", item);
      continue;
    }

    std::string name(item.substr(0, sep));
    std::string value(item.substr(sep + 1));
    if (name == "serveraddr") {
      client_role = (value != "server") && (value != "enb");
    }
    rfsim_config_set(name.c_str(), value.c_str());
  }
}

}

radio_session_rfsimulator_impl::radio_session_rfsimulator_impl(const radio_configuration::radio& config,
                                                               task_executor&        async_task_executor,
                                                               radio_event_notifier& notification_handler) :
  logger(ocudulog::fetch_basic_logger("RF")), device({}), oai_config({})
{
  if ((config.tx_streams.size() != 1) || (config.rx_streams.size() != 1)) {
    logger.error("The rfsimulator radio supports a single transmit and receive stream.");
    return;
  }

  rfsim_set_log_sink(log_sink);
  rfsim_set_log_level(static_cast<int>(config.log_level) >= static_cast<int>(ocudulog::basic_levels::debug) ? 3 : 2);

  rfsim_config_set("serveraddr", "server");
  rfsim_config_set("serverport", DEFAULT_SERVER_PORT);
  apply_device_args(config.args, logger, client_role);

  oai_config.ru_id                       = 0;
  oai_config.tx_num_channels             = config.tx_streams[0].channels.size();
  oai_config.rx_num_channels             = config.rx_streams[0].channels.size();
  oai_config.sample_rate                 = config.sampling_rate_Hz;
  oai_config.tx_bw                       = config.sampling_rate_Hz;
  oai_config.rx_bw                       = config.sampling_rate_Hz;
  oai_config.tx_freq[0]                  = config.tx_streams[0].channels[0].freq.center_frequency_Hz;
  oai_config.rx_freq[0]                  = config.rx_streams[0].channels[0].freq.center_frequency_Hz;
  oai_config.command_line_sample_advance = 0;

  if (device_init(&device, &oai_config) != 0) {
    logger.error("Failed to initialise the rfsimulator.");
    return;
  }

  successful = true;
}

radio_session_rfsimulator_impl::~radio_session_rfsimulator_impl()
{
  if (started) {
    device.trx_end_func(&device);
  }
}

void radio_session_rfsimulator_impl::start(baseband_gateway_timestamp init_time)
{
  ts_offset = init_time;

  if (device.trx_start_func(&device) != 0) {
    logger.error("Failed to start the rfsimulator server.");
    return;
  }
  started = true;

  if (!client_role) {
    logger.info("rfsimulator server listening, waiting for UEs to connect.");
    return;
  }

  unsigned nof_rx_channels = oai_config.rx_num_channels;
  ci16_t   scratch[RADIO_MAX_NOF_CHANNELS];
  void*    buffers[RADIO_MAX_NOF_CHANNELS];
  for (unsigned i_channel = 0; i_channel != nof_rx_channels; ++i_channel) {
    buffers[i_channel] = &scratch[i_channel];
  }
  openair0_timestamp_t ts = 0;
  device.trx_read_func(&device, &ts, buffers, 1, nof_rx_channels);
  ts_offset = init_time - static_cast<baseband_gateway_timestamp>(ts);
  logger.info("rfsimulator client connected, peer clock at {} samples.", ts);
}

void radio_session_rfsimulator_impl::stop()
{

  stopped.store(true);
}

baseband_gateway_receiver::metadata radio_session_rfsimulator_impl::receive(baseband_gateway_buffer_writer& data)
{
  unsigned nof_channels = data.get_nof_channels();
  unsigned nof_samples  = data.get_nof_samples();

  if (stopped.load()) {
    for (unsigned i_channel = 0; i_channel != nof_channels; ++i_channel) {
      ocuduvec::zero(data.get_channel_buffer(i_channel));
    }
    metadata md = {.ts = ts_offset};
    return md;
  }

  void* buffers[RADIO_MAX_NOF_CHANNELS];
  for (unsigned i_channel = 0; i_channel != nof_channels; ++i_channel) {
    buffers[i_channel] = data.get_channel_buffer(i_channel).data();
  }

  openair0_timestamp_t ts = 0;
  device.trx_read_func(&device, &ts, buffers, nof_samples, nof_channels);

  metadata md = {.ts = static_cast<baseband_gateway_timestamp>(ts) + ts_offset};
  return md;
}

void radio_session_rfsimulator_impl::transmit(const baseband_gateway_buffer_reader&        data,
                                              const baseband_gateway_transmitter_metadata& md)
{
  if (stopped.load()) {
    return;
  }

  unsigned nof_channels = data.get_nof_channels();

  void* buffers[RADIO_MAX_NOF_CHANNELS];
  for (unsigned i_channel = 0; i_channel != nof_channels; ++i_channel) {
    buffers[i_channel] = const_cast<ci16_t*>(data.get_channel_buffer(i_channel).data());
  }

  device.trx_write_func(&device,
                        static_cast<openair0_timestamp_t>(md.ts - ts_offset),
                        buffers,
                        data.get_nof_samples(),
                        nof_channels,
                        TX_BURST_START_AND_END);
}
