#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <utility>

struct rte_mbuf;

namespace ocudu {
namespace hal {
namespace cuda {

constexpr unsigned GPU_RX_HEADER_PEEK_BYTES = 64;

class gpu_rx_buffer
{
public:
  gpu_rx_buffer() = default;

  gpu_rx_buffer(::rte_mbuf* mbuf_, std::size_t payload_len_, const std::array<uint8_t, GPU_RX_HEADER_PEEK_BYTES>& hdr) :
    mbuf(mbuf_), payload_len(payload_len_), host_header(hdr)
  {
  }

  ~gpu_rx_buffer();

  gpu_rx_buffer(const gpu_rx_buffer&)            = delete;
  gpu_rx_buffer& operator=(const gpu_rx_buffer&) = delete;

  gpu_rx_buffer(gpu_rx_buffer&& other) noexcept :
    mbuf(other.mbuf), payload_len(other.payload_len), host_header(other.host_header)
  {
    other.mbuf = nullptr;
  }

  gpu_rx_buffer& operator=(gpu_rx_buffer&& other) noexcept
  {
    if (this != &other) {
      release();
      mbuf            = other.mbuf;
      payload_len     = other.payload_len;
      host_header     = other.host_header;
      other.mbuf      = nullptr;
    }
    return *this;
  }

  void* gpu_payload() const;

  std::size_t length() const { return payload_len; }

  const std::array<uint8_t, GPU_RX_HEADER_PEEK_BYTES>& header() const { return host_header; }

  explicit operator bool() const { return mbuf != nullptr; }

private:
  void release();

  ::rte_mbuf*                                       mbuf        = nullptr;
  std::size_t                                       payload_len = 0;
  std::array<uint8_t, GPU_RX_HEADER_PEEK_BYTES>     host_header{};
};

}
}
}
