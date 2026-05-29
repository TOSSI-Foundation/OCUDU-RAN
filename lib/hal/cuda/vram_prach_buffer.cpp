#include "ocudu/hal/cuda/vram_prach_buffer.h"

#include <cstdio>
#include <cstring>
#include <cuda_runtime.h>
#include <new>

namespace ocudu {
namespace hal {
namespace cuda {

namespace {

constexpr std::size_t CBF16_BYTES = 4;

std::size_t total_elements(const vram_prach_buffer::config& cfg)
{
  return static_cast<std::size_t>(cfg.nof_ports) * cfg.nof_td_occasions * cfg.nof_fd_occasions *
         cfg.nof_symbols * cfg.sequence_length;
}

}

std::unique_ptr<vram_prach_buffer> vram_prach_buffer::create(const config& cfg)
{
  if (cfg.nof_ports == 0 || cfg.nof_td_occasions == 0 || cfg.nof_fd_occasions == 0 ||
      cfg.nof_symbols == 0 || cfg.sequence_length == 0) {
    std::fprintf(stderr,
                 "[vram_prach_buffer] invalid config: ports=%u td=%u fd=%u sym=%u seq=%u (all must be > 0)\n",
                 cfg.nof_ports,
                 cfg.nof_td_occasions,
                 cfg.nof_fd_occasions,
                 cfg.nof_symbols,
                 cfg.sequence_length);
    return nullptr;
  }

  const std::size_t bytes = total_elements(cfg) * CBF16_BYTES;
  void*             ptr   = nullptr;
  if (cudaError_t e = cudaMalloc(&ptr, bytes); e != cudaSuccess) {
    std::fprintf(stderr,
                 "[vram_prach_buffer] cudaMalloc(%zu bytes) failed: %s\n",
                 bytes,
                 cudaGetErrorString(e));
    return nullptr;
  }

  if (cudaError_t e = cudaMemset(ptr, 0, bytes); e != cudaSuccess) {
    std::fprintf(stderr,
                 "[vram_prach_buffer] cudaMemset(zero) failed: %s\n",
                 cudaGetErrorString(e));
    cudaFree(ptr);
    return nullptr;
  }

  std::unique_ptr<vram_prach_buffer> obj{new (std::nothrow) vram_prach_buffer()};
  if (!obj) {
    cudaFree(ptr);
    return nullptr;
  }
  obj->cfg         = cfg;
  obj->d_base      = ptr;
  obj->total_bytes = bytes;

  std::fprintf(stderr,
               "[vram_prach_buffer] allocated: ports=%u td=%u fd=%u sym=%u seq=%u total=%zu B at %p\n",
               cfg.nof_ports,
               cfg.nof_td_occasions,
               cfg.nof_fd_occasions,
               cfg.nof_symbols,
               cfg.sequence_length,
               bytes,
               ptr);
  return obj;
}

vram_prach_buffer::~vram_prach_buffer()
{
  if (d_base != nullptr) {

    cudaFree(d_base);
    d_base = nullptr;
  }
}

void* vram_prach_buffer::device_ptr_for_symbol(unsigned i_port,
                                               unsigned i_td_occasion,
                                               unsigned i_fd_occasion,
                                               unsigned i_symbol) const
{

  const std::size_t element_index =
      ((((static_cast<std::size_t>(i_port) * cfg.nof_td_occasions) + i_td_occasion) * cfg.nof_fd_occasions +
        i_fd_occasion) *
           cfg.nof_symbols +
       i_symbol) *
      cfg.sequence_length;
  return static_cast<uint8_t*>(d_base) + element_index * CBF16_BYTES;
}

void vram_prach_buffer::zero_async(void* stream)
{
  if (d_base != nullptr) {
    cudaMemsetAsync(d_base, 0, total_bytes, static_cast<cudaStream_t>(stream));
  }
}

}
}
}
