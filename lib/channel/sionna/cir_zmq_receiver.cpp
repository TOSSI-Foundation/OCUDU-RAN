#include "ocudu/channel/sionna/cir_zmq_receiver.h"
#include "fmt/format.h"
#include <cmath>
#include <cstring>
#include <vector>
#include <zmq.h>

using namespace ocudu;

static constexpr size_t max_message_size = 8 * 1024 * 1024;
static constexpr int receive_timeout_ms = 100;
static constexpr unsigned max_taps      = 64;
static constexpr unsigned max_snapshots = 1000000;

expected<cir_artifact, std::string> ocudu::decode_cir_update(span<const uint8_t> message,
                                                             double              expected_fs_hz,
                                                             unsigned            max_tx_ant,
                                                             unsigned            max_rx_ant)
{
  if (message.size() < sizeof(cir_update_header)) {
    return make_unexpected(fmt::format("Channel update is too short: {} bytes.", message.size()));
  }

  cir_update_header header;
  std::memcpy(&header, message.data(), sizeof(header));

  if (header.magic != cir_update_magic) {
    return make_unexpected(fmt::format("Channel update has an invalid magic 0x{:08x}.", header.magic));
  }
  if (header.version != cir_update_version) {
    return make_unexpected(
        fmt::format("Unsupported channel update version {}, expected {}.", header.version, cir_update_version));
  }
  if ((header.num_snapshots == 0) || (header.num_snapshots > max_snapshots)) {
    return make_unexpected(fmt::format("Channel update declares {} snapshots.", header.num_snapshots));
  }
  if ((header.num_taps == 0) || (header.num_taps > max_taps)) {
    return make_unexpected(fmt::format("Channel update declares {} taps.", header.num_taps));
  }
  if ((header.num_tx_ant == 0) || (header.num_tx_ant > max_tx_ant) || (header.num_rx_ant == 0) ||
      (header.num_rx_ant > max_rx_ant)) {
    return make_unexpected(fmt::format("Channel update declares {}x{} antennas, supported up to {}x{}.",
                                       header.num_rx_ant,
                                       header.num_tx_ant,
                                       max_rx_ant,
                                       max_tx_ant));
  }
  if (!std::isfinite(header.fs_hz) || (std::abs(header.fs_hz - expected_fs_hz) > (1e-6 * expected_fs_hz))) {
    return make_unexpected(fmt::format("Channel update sampling rate {} Hz does not match the radio rate {} Hz.",
                                       header.fs_hz,
                                       expected_fs_hz));
  }
  if ((header.num_snapshots > 1) && (!std::isfinite(header.snapshot_dt_s) || (header.snapshot_dt_s <= 0.0))) {
    return make_unexpected(
        fmt::format("Channel update declares an invalid snapshot period {} s.", header.snapshot_dt_s));
  }

  size_t nof_values = static_cast<size_t>(header.num_snapshots) * header.num_rx_ant * header.num_tx_ant *
                      header.num_taps;
  size_t expected_size = sizeof(cir_update_header) + nof_values * sizeof(cf_t);
  if (message.size() != expected_size) {
    return make_unexpected(fmt::format("Channel update size {} does not match the declared geometry ({} expected).",
                                       message.size(),
                                       expected_size));
  }

  cir_artifact artifact;
  artifact.fs_hz         = header.fs_hz;
  artifact.snapshot_dt_s = header.snapshot_dt_s;
  artifact.nof_snapshots = header.num_snapshots;
  artifact.nof_tx_ant    = header.num_tx_ant;
  artifact.nof_rx_ant    = header.num_rx_ant;
  artifact.nof_taps      = header.num_taps;
  artifact.loop          = (header.loop != 0);
  artifact.normalization = "absolute";
  artifact.scene         = fmt::format("live:{}", header.sequence);
  artifact.taps.resize(nof_values);
  std::memcpy(artifact.taps.data(), message.data() + sizeof(cir_update_header), nof_values * sizeof(cf_t));

  for (const cf_t& tap : artifact.taps) {
    if (!std::isfinite(tap.real()) || !std::isfinite(tap.imag())) {
      return make_unexpected("Channel update contains non-finite tap values.");
    }
  }

  return artifact;
}

cir_zmq_receiver::cir_zmq_receiver(const std::string&                    address,
                                   std::shared_ptr<swappable_cir_source> source_,
                                   double                                expected_fs_hz_,
                                   unsigned                              max_tx_ant_,
                                   unsigned                              max_rx_ant_) :
  logger(ocudulog::fetch_basic_logger("RF", false)),
  source(std::move(source_)),
  expected_fs_hz(expected_fs_hz_),
  max_tx_ant(max_tx_ant_),
  max_rx_ant(max_rx_ant_)
{
  zmq_context = ::zmq_ctx_new();
  if (zmq_context == nullptr) {
    logger.error("Sionna live channel: failed to create the ZMQ context. {}.", ::zmq_strerror(::zmq_errno()));
    return;
  }

  zmq_socket = ::zmq_socket(zmq_context, ZMQ_SUB);
  if (zmq_socket == nullptr) {
    logger.error("Sionna live channel: failed to create the ZMQ socket. {}.", ::zmq_strerror(::zmq_errno()));
    return;
  }

  int timeout = receive_timeout_ms;
  if (::zmq_setsockopt(zmq_socket, ZMQ_RCVTIMEO, &timeout, sizeof(timeout)) == -1) {
    logger.error("Sionna live channel: failed to set the receive timeout. {}.", ::zmq_strerror(::zmq_errno()));
    return;
  }
  int linger = 0;
  ::zmq_setsockopt(zmq_socket, ZMQ_LINGER, &linger, sizeof(linger));
  if (::zmq_setsockopt(zmq_socket, ZMQ_SUBSCRIBE, "", 0) == -1) {
    logger.error("Sionna live channel: failed to subscribe. {}.", ::zmq_strerror(::zmq_errno()));
    return;
  }
  if (::zmq_connect(zmq_socket, address.c_str()) == -1) {
    logger.error("Sionna live channel: failed to connect to '{}'. {}.", address, ::zmq_strerror(::zmq_errno()));
    return;
  }

  running    = true;
  rx_thread  = unique_thread("sionna_cir", [this]() { receive_loop(); });
  successful = true;
  logger.info("Sionna live channel: subscribed to '{}'.", address);
}

cir_zmq_receiver::~cir_zmq_receiver()
{
  running = false;
  if (rx_thread.running()) {
    rx_thread.join();
  }
  if (zmq_socket != nullptr) {
    ::zmq_close(zmq_socket);
  }
  if (zmq_context != nullptr) {
    ::zmq_ctx_shutdown(zmq_context);
    ::zmq_ctx_destroy(zmq_context);
  }
}

void cir_zmq_receiver::receive_loop()
{
  std::vector<uint8_t> buffer(max_message_size);

  while (running.load(std::memory_order_relaxed)) {
    int nof_bytes = ::zmq_recv(zmq_socket, buffer.data(), buffer.size(), 0);
    if (nof_bytes < 0) {
      // Timeouts are expected: they let the loop observe the stop request.
      if (::zmq_errno() != EAGAIN) {
        logger.warning("Sionna live channel: receive failed. {}.", ::zmq_strerror(::zmq_errno()));
      }
      continue;
    }

    expected<cir_artifact, std::string> artifact =
        decode_cir_update(span<const uint8_t>(buffer.data(), nof_bytes), expected_fs_hz, max_tx_ant, max_rx_ant);
    if (!artifact.has_value()) {
      rejected.fetch_add(1, std::memory_order_relaxed);
      logger.warning("Sionna live channel: discarded update. {}", artifact.error());
      continue;
    }

    unsigned nof_snapshots = artifact->nof_snapshots;
    unsigned nof_taps      = artifact->nof_taps;
    source->publish(std::make_shared<const cir_artifact>(std::move(*artifact)));
    uint64_t total = accepted.fetch_add(1, std::memory_order_relaxed) + 1;
    logger.debug("Sionna live channel: applied update {} ({} snapshot(s), {} tap(s)).",
                 total,
                 nof_snapshots,
                 nof_taps);
  }
}
