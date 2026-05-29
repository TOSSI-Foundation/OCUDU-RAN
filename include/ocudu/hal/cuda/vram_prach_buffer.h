#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>

namespace ocudu {
namespace hal {
namespace cuda {

class vram_prach_buffer
{
public:
  struct config {
    unsigned nof_ports;
    unsigned nof_td_occasions;
    unsigned nof_fd_occasions;
    unsigned nof_symbols;
    unsigned sequence_length;
  };

  static std::unique_ptr<vram_prach_buffer> create(const config& cfg);

  ~vram_prach_buffer();

  vram_prach_buffer(const vram_prach_buffer&)            = delete;
  vram_prach_buffer& operator=(const vram_prach_buffer&) = delete;

  void* device_ptr_for_symbol(unsigned i_port,
                              unsigned i_td_occasion,
                              unsigned i_fd_occasion,
                              unsigned i_symbol) const;

  void* device_base() const { return d_base; }

  std::size_t size_bytes() const { return total_bytes; }

  const config& get_config() const { return cfg; }

  void zero_async(void* stream);

private:
  vram_prach_buffer() = default;

  config       cfg{};
  void*        d_base      = nullptr;
  std::size_t  total_bytes = 0;
};

}
}
}
