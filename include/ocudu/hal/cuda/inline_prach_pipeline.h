#pragma once

#include "ocudu/hal/cuda/prach_detector_inline.h"
#include "ocudu/hal/cuda/prach_inline_bfp_kernel.h"
#include "ocudu/hal/cuda/vram_prach_buffer.h"
#include "ocudu/phy/upper/channel_processors/prach/prach_detector.h"
#include "ocudu/phy/upper/channel_processors/prach/prach_detection_result.h"

#include <cstdint>
#include <functional>
#include <memory>
#include <vector>

namespace ocudu {
namespace hal {
namespace cuda {

class inline_prach_pipeline
{
public:
  struct config {

    unsigned nof_prbs_per_packet;

    unsigned k_bar;

    unsigned prb_bytes;

    unsigned data_width;

    float quantizer_gain;

    unsigned L_ra;

    prach_detector::configuration detector_cfg;

    unsigned nof_prach_eaxc;
    unsigned nof_prach_symbols;

    prach_detector_inline_config detector_inline_cfg;

    unsigned slot_ring_depth = 4;

    std::function<void(const prach_detection_result& result, uint32_t system_slot_id)> on_result_cb;
  };

  static std::unique_ptr<inline_prach_pipeline> create(const config& cfg);

  virtual ~inline_prach_pipeline() = default;

  virtual void on_packet(uint32_t                  system_slot_id,
                         unsigned                  symbol_idx,
                         unsigned                  eaxc_port_idx,
                         const void*               d_compressed_bytes,
                         std::function<void()>     mbuf_release) = 0;

  virtual void flush_stale_slots(uint32_t system_slot_id, unsigned keepalive_slots) = 0;
};

}
}
}
