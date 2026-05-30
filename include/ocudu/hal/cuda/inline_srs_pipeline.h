#pragma once

#include "ocudu/hal/cuda/srs_estimator_inline.h"
#include "ocudu/hal/cuda/srs_inline_bfp_kernel.h"
#include "ocudu/hal/cuda/vram_srs_buffer.h"
#include "ocudu/phy/upper/signal_processors/srs/srs_estimator_result.h"
#include "ocudu/ran/srs/srs_resource_configuration.h"

#include <cstdint>
#include <functional>
#include <memory>

namespace ocudu {
namespace hal {
namespace cuda {

class inline_srs_pipeline
{
public:
  struct config {

    unsigned prb_bytes;

    unsigned data_width;

    float quantizer_gain;

    unsigned cell_nof_prbs;

    unsigned numerology;

    unsigned max_nof_rx_ports;
    unsigned max_nof_symbols;
    unsigned max_sequence_length;

    unsigned slot_ring_depth = 4;

    std::function<void(const srs_estimator_result& result, uint32_t system_slot_id)> on_result_cb;
  };

  static std::unique_ptr<inline_srs_pipeline> create(const config& cfg);

  virtual ~inline_srs_pipeline() = default;

  virtual void on_packet(uint32_t                          system_slot_id,
                         unsigned                          symbol_idx,
                         unsigned                          rx_port_idx,
                         unsigned                          nof_prbs,
                         unsigned                          start_prb,
                         const srs_resource_configuration& resource,
                         unsigned                          nof_rx_ports,
                         const void*                       d_compressed_bytes,
                         std::function<void()>             mbuf_release) = 0;

  virtual void flush_stale_slots(uint32_t system_slot_id, unsigned keepalive_slots) = 0;
};

}
}
}
