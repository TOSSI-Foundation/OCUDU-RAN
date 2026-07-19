

#include "radio_factory_sionna_impl.h"
#include "radio_session_sionna_impl.h"
#include "../zmq/radio_config_zmq_validator.h"
#include "../zmq/radio_factory_zmq_impl.h"
#include "ocudu/channel/sionna/cir_artifact.h"
#include "ocudu/channel/sionna/cir_source.h"
#include "ocudu/channel/sionna/cir_zmq_receiver.h"
#include "ocudu/ocudulog/ocudulog.h"
#include <algorithm>
#include <cmath>
#include <memory>
#include <string>
#include <vector>

using namespace ocudu;

/// Maximum number of samples processed in a single transmit call.
static constexpr unsigned default_max_block_size = 61440;
/// Relative tolerance when comparing the artifact sampling rate against the radio sampling rate.
static constexpr double srate_relative_tolerance = 1e-6;
/// Maximum number of taps any channel may declare, bounding the preallocated filter history.
static constexpr unsigned max_supported_taps = 64;
/// Maximum number of antennas accepted from a live channel update.
static constexpr unsigned max_supported_antennas = 4;

namespace {

/// Splits the device arguments into comma separated entries.
std::vector<std::string> split_args(const std::string& args)
{
  std::vector<std::string> entries;
  size_t                   start = 0;
  while (start <= args.size()) {
    size_t end = args.find(',', start);
    if (end == std::string::npos) {
      end = args.size();
    }
    std::string entry = args.substr(start, end - start);
    if (!entry.empty()) {
      entries.push_back(entry);
    }
    start = end + 1;
  }
  return entries;
}

/// Extracts the value of a \c key=value entry from the device arguments. Returns an empty string if absent.
std::string get_arg_value(const std::string& args, const std::string& key)
{
  for (const std::string& entry : split_args(args)) {
    size_t sep = entry.find('=');
    if (sep == std::string::npos) {
      continue;
    }
    if (entry.substr(0, sep) == key) {
      return entry.substr(sep + 1);
    }
  }
  return {};
}

} // namespace

std::unique_ptr<radio_session> radio_factory_sionna_impl::create(const radio_configuration::radio& config,
                                                                 task_executor&                    async_task_executor,
                                                                 radio_event_notifier&             notifier)
{
  ocudulog::basic_logger& logger = ocudulog::fetch_basic_logger("RF", false);

  std::string manifest_path = get_arg_value(config.args, "cir");
  std::string live_address  = get_arg_value(config.args, "live");
  if (manifest_path.empty() && live_address.empty()) {
    logger.error("Sionna radio requires a channel source in the device arguments: 'cir=/path/manifest.json' for an "
                 "artifact, 'live=tcp://host:port' for live updates.");
    return nullptr;
  }

  unsigned nof_tx_channels = 0;
  for (const radio_configuration::stream& stream : config.tx_streams) {
    nof_tx_channels = std::max<unsigned>(nof_tx_channels, stream.channels.size());
  }

  auto source = std::make_shared<swappable_cir_source>();

  if (!manifest_path.empty()) {
    expected<cir_artifact, std::string> artifact = load_cir_artifact(manifest_path);
    if (!artifact.has_value()) {
      logger.error("Failed to load the Sionna CIR artifact: {}", artifact.error());
      return nullptr;
    }

    // The artifact taps are only valid for the sampling rate they were generated for.
    double srate_error = std::abs(artifact->fs_hz - config.sampling_rate_Hz);
    if (srate_error > (srate_relative_tolerance * config.sampling_rate_Hz)) {
      logger.error(
          "Sionna CIR artifact sampling rate (i.e., {} Hz) does not match the radio sampling rate (i.e., {} Hz).",
          artifact->fs_hz,
          config.sampling_rate_Hz);
      return nullptr;
    }

    if (nof_tx_channels > artifact->nof_tx_ant) {
      logger.error("Sionna CIR artifact provides {} transmit antennas but the radio configures {} channels.",
                   artifact->nof_tx_ant,
                   nof_tx_channels);
      return nullptr;
    }

    logger.info("Sionna channel enabled: scene='{}', {} snapshot(s), {} tap(s), {}x{} antennas, normalization '{}'.",
                artifact->scene,
                artifact->nof_snapshots,
                artifact->nof_taps,
                artifact->nof_rx_ant,
                artifact->nof_tx_ant,
                artifact->normalization);

    source->publish(std::make_shared<const cir_artifact>(std::move(*artifact)));
  }

  std::unique_ptr<cir_zmq_receiver> receiver;
  if (!live_address.empty()) {
    receiver = std::make_unique<cir_zmq_receiver>(
        live_address, source, config.sampling_rate_Hz, max_supported_antennas, max_supported_antennas);
    if (!receiver->is_successful()) {
      logger.error("Failed to start the Sionna live channel receiver on '{}'.", live_address);
      return nullptr;
    }
    if (manifest_path.empty()) {
      logger.info("Sionna live channel: waiting for the first update, samples pass through until then.");
    }
  }

  radio_factory_zmq_impl         transport_factory;
  std::unique_ptr<radio_session> transport = transport_factory.create(config, async_task_executor, notifier);
  if (!transport) {
    logger.error("Failed to create the transport radio session for the Sionna radio.");
    return nullptr;
  }

  return std::make_unique<radio_session_sionna_impl>(std::move(transport),
                                                     std::move(source),
                                                     std::move(receiver),
                                                     config,
                                                     default_max_block_size,
                                                     max_supported_taps);
}

const radio_configuration::validator& radio_factory_sionna_impl::get_configuration_validator() const
{
  static radio_config_zmq_config_validator config_validator;
  return config_validator;
}
