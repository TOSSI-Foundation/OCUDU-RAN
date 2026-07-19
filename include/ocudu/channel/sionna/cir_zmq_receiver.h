#pragma once

#include "ocudu/channel/sionna/cir_artifact.h"
#include "ocudu/channel/sionna/cir_source.h"
#include "ocudu/adt/expected.h"
#include "ocudu/adt/span.h"
#include "ocudu/ocudulog/ocudulog.h"
#include "ocudu/support/executors/unique_thread.h"
#include <atomic>
#include <cstdint>
#include <memory>
#include <string>

namespace ocudu {

struct cir_update_header {
  /// Magic identifier, must be \c cir_update_magic.
  uint32_t magic;
  /// Wire format version, must be \c cir_update_version.
  uint32_t version;
  uint32_t sequence;
  uint32_t num_snapshots;
  uint32_t num_tx_ant;
  uint32_t num_rx_ant;
  uint32_t num_taps;
  uint32_t loop;
  double fs_hz;
  double snapshot_dt_s;
};

static constexpr uint32_t cir_update_magic = 0x4F434952;
static constexpr uint32_t cir_update_version = 1;

expected<cir_artifact, std::string> decode_cir_update(span<const uint8_t> message,
                                                      double              expected_fs_hz,
                                                      unsigned            max_tx_ant,
                                                      unsigned            max_rx_ant);


class cir_zmq_receiver
{
public:
  cir_zmq_receiver(const std::string&                     address,
                   std::shared_ptr<swappable_cir_source>  source,
                   double                                 expected_fs_hz,
                   unsigned                               max_tx_ant,
                   unsigned                               max_rx_ant);

  ~cir_zmq_receiver();

  bool is_successful() const { return successful; }

  uint64_t get_nof_accepted() const { return accepted.load(std::memory_order_relaxed); }

  uint64_t get_nof_rejected() const { return rejected.load(std::memory_order_relaxed); }

private:
  void receive_loop();

  ocudulog::basic_logger&               logger;
  std::shared_ptr<swappable_cir_source> source;
  double                                expected_fs_hz;
  unsigned                              max_tx_ant;
  unsigned                              max_rx_ant;
  void*                                 zmq_context = nullptr;
  void*                                 zmq_socket  = nullptr;
  std::atomic<bool>                     running     = {false};
  std::atomic<uint64_t>                 accepted    = {0};
  std::atomic<uint64_t>                 rejected    = {0};
  bool                                  successful  = false;
  unique_thread                         rx_thread;
};

} // namespace ocudu
