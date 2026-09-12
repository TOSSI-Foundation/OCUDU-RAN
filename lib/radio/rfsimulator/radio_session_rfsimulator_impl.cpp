// SPDX-FileCopyrightText: Copyright (C) 2021-2026 Software Radio Systems Limited
// SPDX-License-Identifier: BSD-3-Clause-Open-MPI

#include "radio_session_rfsimulator_impl.h"
#include "rfsim_shim.h"
#include "ocudu/gateways/baseband/buffer/baseband_gateway_buffer_reader.h"
#include "ocudu/gateways/baseband/buffer/baseband_gateway_buffer_writer.h"
#include "ocudu/ocudulog/ocudulog.h"
#include "ocudu/ocuduvec/zero.h"
#include <algorithm>
#include <cmath>
#include <string>
#include <thread>

using namespace ocudu;

static_assert(sizeof(ci16_t) == sizeof(c16_t), "Baseband buffers must match the rfsimulator sample format");

static constexpr const char* DEFAULT_SERVER_PORT = "4043";

namespace {

void log_sink(int level, const char* message)
{
  static ocudulog::basic_logger& log = ocudulog::fetch_basic_logger("RF");

  std::string_view view(message);
  while (!view.empty() && (view.back() == '\n')) {
    view.remove_suffix(1);
  }
  std::string text(view);

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

void apply_device_args(const std::string&      args,
                       ocudulog::basic_logger& logger,
                       bool&                   client_role,
                       bool&                   realtime,
                       bool&                   channel_model_enabled,
                       bool&                   rx_trace)
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

    if (item == "realtime") {
      realtime = true;
      continue;
    }
    if (item == "rxtrace") {
      rx_trace = true;
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
    if (name == "options") {
      channel_model_enabled = (value.find("chanmod") != std::string::npos);
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
  bool realtime = false;
  apply_device_args(config.args, logger, client_role, realtime, channel_model_enabled, rx_trace_enabled);
  if (realtime && std::isfinite(config.sampling_rate_Hz) && (config.sampling_rate_Hz > 0.0)) {
    pace_sample_period_s = 1.0 / config.sampling_rate_Hz;
    pace_enabled         = true;
    logger.info("rfsimulator paced to real time at {:.3f} Msps.", config.sampling_rate_Hz / 1e6);
  }

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

bool radio_session_rfsimulator_impl::set_ntn_channel(double one_way_delay_us,
                                                     double delay_drift_us_per_s,
                                                     double service_drift_us_per_s,
                                                     bool   emulate_doppler,
                                                     bool   link_up)
{
  ntn_link_up.store(link_up, std::memory_order_relaxed);

  if (!channel_model_enabled) {
    return false;
  }

  rfsim_set_ntn_channel(one_way_delay_us, delay_drift_us_per_s, link_up);

  ntn_doppler_enabled.store(emulate_doppler, std::memory_order_relaxed);
  if (emulate_doppler) {
    const double one_way_rate = -(0.5 * service_drift_us_per_s * 1e-6);
    rfsim_set_ntn_doppler(one_way_rate * oai_config.rx_freq[0]);
    tx_doppler_hz.store(one_way_rate * oai_config.tx_freq[0], std::memory_order_relaxed);
  }
  return true;
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
  if (pace_enabled && (pace_total_count != 0)) {
    const double late_pct = 100.0 * static_cast<double>(pace_late_count) / pace_total_count;
    const double slip_ms  = std::chrono::duration<double, std::milli>(pace_max_slip).count();
    if (pace_late_count != 0) {
      logger.warning("rfsimulator real-time pacing: {} of {} transmit blocks were late ({:.1f}%), worst slip "
                     "{:.3f} ms.",
                     pace_late_count,
                     pace_total_count,
                     late_pct,
                     slip_ms);
    } else {
      logger.info("rfsimulator real-time pacing: held real time for all {} transmit blocks.", pace_total_count);
    }
  }

  stopped.store(true);
}

void radio_session_rfsimulator_impl::apply_tx_doppler(span<ci16_t> samples)
{
  const double doppler_hz = tx_doppler_hz.load(std::memory_order_relaxed);
  if ((doppler_hz == 0.0) || (oai_config.sample_rate <= 0.0)) {
    return;
  }

  const double phase_inc = 2.0 * M_PI * doppler_hz / oai_config.sample_rate;
  const double step_re   = std::cos(phase_inc);
  const double step_im   = std::sin(phase_inc);

  double rot_re = std::cos(tx_doppler_phase);
  double rot_im = std::sin(tx_doppler_phase);

  for (unsigned i = 0, e = samples.size(); i != e; ++i) {
    const double in_re = samples[i].real();
    const double in_im = samples[i].imag();

    samples[i] = {static_cast<int16_t>(std::lround(std::clamp(in_re * rot_re - in_im * rot_im, -32768.0, 32767.0))),
                  static_cast<int16_t>(std::lround(std::clamp(in_re * rot_im + in_im * rot_re, -32768.0, 32767.0)))};

    const double next_re = rot_re * step_re - rot_im * step_im;
    rot_im               = rot_re * step_im + rot_im * step_re;
    rot_re               = next_re;

    if ((i & 0x1fffU) == 0x1fffU) {
      const double inv = 1.0 / std::sqrt(rot_re * rot_re + rot_im * rot_im);
      rot_re *= inv;
      rot_im *= inv;
    }
  }

  tx_doppler_phase = std::fmod(tx_doppler_phase + phase_inc * samples.size(), 2.0 * M_PI);
}

void radio_session_rfsimulator_impl::pace_to_timestamp(baseband_gateway_timestamp ts)
{
  if (!pace_origin_valid) {
    pace_origin_tp    = std::chrono::steady_clock::now();
    pace_origin_ts    = ts;
    pace_origin_valid = true;
    return;
  }

  if (ts <= pace_origin_ts) {
    return;
  }

  const double elapsed_s = static_cast<double>(ts - pace_origin_ts) * pace_sample_period_s;
  const auto   deadline  = pace_origin_tp + std::chrono::duration_cast<std::chrono::steady_clock::duration>(
                                             std::chrono::duration<double>(elapsed_s));
  const auto now = std::chrono::steady_clock::now();

  ++pace_total_count;

  if (now >= deadline) {
    const auto slip = std::chrono::duration_cast<std::chrono::nanoseconds>(now - deadline);
    if (slip > pace_max_slip) {
      pace_max_slip = slip;
    }
    ++pace_late_count;
    return;
  }

  std::this_thread::sleep_until(deadline);
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
  int                  nread = device.trx_read_func(&device, &ts, buffers, nof_samples, nof_channels);

  if (rx_trace_enabled) {
    span<const ci16_t> ch0    = data.get_channel_buffer(0);
    int32_t            peak   = 0;
    int64_t            energy = 0;
    for (unsigned i = 0, e = ch0.size(); i != e; ++i) {
      const int32_t re = ch0[i].real();
      const int32_t im = ch0[i].imag();
      const int32_t m2 = re * re + im * im;
      energy += m2;
      peak = std::max(peak, m2);
    }
    const double rms = ch0.empty() ? 0.0 : std::sqrt(static_cast<double>(energy) / ch0.size());
    rx_trace_peak    = std::max(rx_trace_peak, std::sqrt(static_cast<double>(peak)) / 32768.0);
    rx_trace_rms     = std::max(rx_trace_rms, rms / 32768.0);
    rx_trace_short += (nread > 0 && static_cast<unsigned>(nread) < nof_samples) ? 1 : 0;
    if (++rx_trace_count % 500 == 0) {
      logger.info("rxtrace: asked {} got {} short {} of 500  peak_amp {:.5f} rms {:.5f}",
                  nof_samples,
                  nread,
                  rx_trace_short,
                  rx_trace_peak,
                  rx_trace_rms);
      rx_trace_peak  = 0.0;
      rx_trace_rms   = 0.0;
      rx_trace_short = 0;
    }
  }

  metadata md = {.ts = static_cast<baseband_gateway_timestamp>(ts) + ts_offset};
  return md;
}

void radio_session_rfsimulator_impl::transmit(const baseband_gateway_buffer_reader&        data,
                                              const baseband_gateway_transmitter_metadata& md)
{
  if (stopped.load()) {
    return;
  }

  if (pace_enabled) {
    pace_to_timestamp(md.ts);
  }

  unsigned nof_channels = data.get_nof_channels();
  unsigned nof_samples  = data.get_nof_samples();

  void* buffers[RADIO_MAX_NOF_CHANNELS];

  const bool link_up = ntn_link_up.load(std::memory_order_relaxed);

  if (!link_up) {
    tx_scratch.resize(nof_channels);
    for (unsigned i_channel = 0; i_channel != nof_channels; ++i_channel) {
      tx_scratch[i_channel].assign(data.get_channel_buffer(i_channel).size(), ci16_t{});
      buffers[i_channel] = tx_scratch[i_channel].data();
    }
  } else if (ntn_doppler_enabled.load(std::memory_order_relaxed)) {
    tx_scratch.resize(nof_channels);
    for (unsigned i_channel = 0; i_channel != nof_channels; ++i_channel) {
      span<const ci16_t> in = data.get_channel_buffer(i_channel);
      tx_scratch[i_channel].assign(in.begin(), in.end());
      buffers[i_channel] = tx_scratch[i_channel].data();
    }
    const double phase_before = tx_doppler_phase;
    for (unsigned i_channel = 0; i_channel != nof_channels; ++i_channel) {
      tx_doppler_phase = phase_before;
      apply_tx_doppler(tx_scratch[i_channel]);
    }
  } else {
    for (unsigned i_channel = 0; i_channel != nof_channels; ++i_channel) {
      buffers[i_channel] = const_cast<ci16_t*>(data.get_channel_buffer(i_channel).data());
    }
  }

  device.trx_write_func(&device,
                        static_cast<openair0_timestamp_t>(md.ts - ts_offset),
                        buffers,
                        nof_samples,
                        nof_channels,
                        TX_BURST_START_AND_END);
}
