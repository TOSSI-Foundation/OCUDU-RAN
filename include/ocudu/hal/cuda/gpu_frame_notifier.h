#pragma once

#include "ocudu/hal/cuda/gpu_rx_buffer.h"

namespace ocudu {
namespace hal {
namespace cuda {

class gpu_frame_notifier
{
public:
  virtual ~gpu_frame_notifier() = default;

  virtual void on_new_gpu_frame(gpu_rx_buffer buffer) = 0;
};

}
}
}
