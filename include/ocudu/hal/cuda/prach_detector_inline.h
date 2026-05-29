#pragma once

#include "ocudu/hal/cuda/vram_prach_buffer.h"
#include "ocudu/phy/upper/channel_processors/prach/prach_detection_result.h"
#include "ocudu/phy/upper/channel_processors/prach/prach_detector.h"

#include <memory>

namespace ocudu {
namespace hal {
namespace cuda {

class prach_detector_inline
{
public:
  virtual ~prach_detector_inline() = default;

  virtual prach_detection_result detect_inline(const vram_prach_buffer&             input,
                                               const prach_detector::configuration& config) = 0;
};

struct prach_detector_inline_config {

  unsigned idft_long_size = 1024;

  unsigned idft_short_size = 256;

  unsigned max_batch = 16;
};

std::unique_ptr<prach_detector_inline> create_prach_detector_inline(const prach_detector_inline_config& cfg = {});

}
}
}
