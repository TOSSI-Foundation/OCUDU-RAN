#include "ocudu/hal/cuda/gpu_rx_buffer.h"

#include <rte_mbuf.h>

namespace ocudu {
namespace hal {
namespace cuda {

gpu_rx_buffer::~gpu_rx_buffer()
{
  release();
}

void gpu_rx_buffer::release()
{
  if (mbuf != nullptr) {
    ::rte_pktmbuf_free(mbuf);
    mbuf = nullptr;
  }
}

void* gpu_rx_buffer::gpu_payload() const
{
  if (mbuf == nullptr) {
    return nullptr;
  }
  return static_cast<uint8_t*>(mbuf->buf_addr) + mbuf->data_off;
}

}
}
}
