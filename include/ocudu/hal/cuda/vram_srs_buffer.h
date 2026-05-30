#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>

namespace ocudu {
namespace hal {
namespace cuda {

class vram_srs_buffer
{
public:
  struct config {
    unsigned nof_rx_ports;
    unsigned nof_symbols;
    unsigned sequence_length;

  };

  static std::unique_ptr<vram_srs_buffer> create(const config& cfg);

  ~vram_srs_buffer();

  vram_srs_buffer(const vram_srs_buffer&)            = delete;
  vram_srs_buffer& operator=(const vram_srs_buffer&) = delete;

  void* device_ptr_for_symbol(unsigned i_port, unsigned i_symbol) const;

  void* device_base() const { return d_base; }

  std::size_t size_bytes() const { return total_bytes; }

  const config& get_config() const { return cfg; }

  void zero_async(void* stream);

private:
  vram_srs_buffer() = default;

  config       cfg{};
  void*        d_base      = nullptr;
  std::size_t  total_bytes = 0;
};

}
}
}
