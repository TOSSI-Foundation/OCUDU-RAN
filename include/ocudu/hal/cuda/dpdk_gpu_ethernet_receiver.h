#pragma once

#include "ocudu/hal/cuda/gpu_frame_notifier.h"

#include <atomic>
#include <cstdint>

namespace ocudu {
namespace hal {
namespace cuda {

class dpdk_gpu_ethernet_receiver
{
public:
  static constexpr unsigned MAX_BURST = 32;

  dpdk_gpu_ethernet_receiver(uint16_t port_id_, uint16_t queue_id_, gpu_frame_notifier& notifier_);

  unsigned receive_once();

  uint64_t total_frames() const { return frames_delivered.load(std::memory_order_relaxed); }

  uint64_t header_peek_failures() const { return peek_failures.load(std::memory_order_relaxed); }

private:
  uint16_t              port_id;
  uint16_t              queue_id;
  gpu_frame_notifier&   notifier;
  std::atomic<uint64_t> frames_delivered{0};
  std::atomic<uint64_t> peek_failures{0};
};

}
}
}
