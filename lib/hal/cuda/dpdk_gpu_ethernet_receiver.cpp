#include "ocudu/hal/cuda/dpdk_gpu_ethernet_receiver.h"

#include <algorithm>
#include <array>
#include <cstdio>

#include <cuda_runtime.h>
#include <rte_ethdev.h>
#include <rte_mbuf.h>

namespace ocudu {
namespace hal {
namespace cuda {

dpdk_gpu_ethernet_receiver::dpdk_gpu_ethernet_receiver(uint16_t            port_id_,
                                                       uint16_t            queue_id_,
                                                       gpu_frame_notifier& notifier_) :
  port_id(port_id_), queue_id(queue_id_), notifier(notifier_)
{
}

unsigned dpdk_gpu_ethernet_receiver::receive_once()
{
  std::array<::rte_mbuf*, MAX_BURST> mbufs;
  const unsigned                     nb = ::rte_eth_rx_burst(port_id, queue_id, mbufs.data(), MAX_BURST);
  if (nb == 0) {
    return 0;
  }

  for (unsigned i = 0; i < nb; ++i) {
    ::rte_mbuf* m = mbufs[i];

    void* const         vram_data   = static_cast<uint8_t*>(m->buf_addr) + m->data_off;
    const std::size_t   payload_len = m->data_len;
    const std::size_t   peek_len    = std::min<std::size_t>(payload_len, GPU_RX_HEADER_PEEK_BYTES);

    std::array<uint8_t, GPU_RX_HEADER_PEEK_BYTES> host_header{};
    cudaError_t err = cudaMemcpy(host_header.data(), vram_data, peek_len, cudaMemcpyDeviceToHost);
    if (err != cudaSuccess) {
      peek_failures.fetch_add(1, std::memory_order_relaxed);

      ::rte_pktmbuf_free(m);
      continue;
    }

    notifier.on_new_gpu_frame(gpu_rx_buffer{m, payload_len, host_header});
    frames_delivered.fetch_add(1, std::memory_order_relaxed);
  }
  return nb;
}

}
}
}
