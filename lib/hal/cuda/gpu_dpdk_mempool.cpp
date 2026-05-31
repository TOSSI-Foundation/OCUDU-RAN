#include "ocudu/hal/cuda/gpu_dpdk_mempool.h"

#include <cstdio>
#include <cstring>
#include <new>

#define ALLOW_EXPERIMENTAL_API

#include <cuda_runtime.h>
#include <rte_errno.h>
#include <rte_ethdev.h>
#include <rte_gpudev.h>
#include <rte_malloc.h>
#include <rte_mbuf.h>
#include <rte_memory.h>
#include <rte_mempool.h>

namespace ocudu {
namespace hal {
namespace cuda {

namespace {

bool ensure_gpudev_initialized()
{

  if (rte_gpu_count_avail() == 0) {
    if (int rc = rte_gpu_init( 16); rc < 0) {
      std::fprintf(stderr, "[gpu_dpdk_mempool] rte_gpu_init failed: rc=%d errno=%d (%s)\n", rc, rte_errno, strerror(rte_errno));
      return false;
    }
  }
  if (rte_gpu_count_avail() == 0) {
    std::fprintf(stderr,
                 "[gpu_dpdk_mempool] rte_gpu_count_avail() == 0. No DPDK gpudev backend registered. "
                 "Confirm DPDK was built with gpu/cuda driver and EAL was started with -a for the GPU.\n");
    return false;
  }
  return true;
}

}

std::unique_ptr<gpu_dpdk_mempool> gpu_dpdk_mempool::create(const gpu_dpdk_mempool_config& cfg)
{
  if (!ensure_gpudev_initialized()) {
    return nullptr;
  }

  static bool s_banner_printed = false;
  if (!s_banner_printed) {
    s_banner_printed = true;
    int driver_v = 0, runtime_v = 0;
    cudaDriverGetVersion(&driver_v);
    cudaRuntimeGetVersion(&runtime_v);
    cudaDeviceProp prop{};
    if (cudaGetDeviceProperties(&prop, 0) == cudaSuccess) {
      const std::size_t vram_mb = static_cast<std::size_t>(prop.totalGlobalMem) / (1024UL * 1024UL);
      int clock_khz = 0;
      cudaDeviceGetAttribute(&clock_khz, cudaDevAttrClockRate, 0);
      std::fprintf(stderr,
                   "[gpu_init] CUDA driver=%d.%d runtime=%d.%d device='%s' sm_%d%d "
                   "sm_count=%d vram=%zu MiB l2=%d KiB clock=%d MHz\n",
                   driver_v / 1000, (driver_v % 100) / 10,
                   runtime_v / 1000, (runtime_v % 100) / 10,
                   prop.name, prop.major, prop.minor,
                   prop.multiProcessorCount, vram_mb,
                   prop.l2CacheSize / 1024,
                   clock_khz / 1000);
    }
    std::fprintf(stderr,
                 "[gpu_init] DPDK gpudev backend devs=%u (using dev_id=%d) total_mbufs=%u data_room=%u B\n",
                 rte_gpu_count_avail(), cfg.gpu_dev_id, cfg.nb_mbufs, cfg.data_room_size);
  }

  const uint16_t nof_gpus = rte_gpu_count_avail();
  if (cfg.gpu_dev_id < 0 || cfg.gpu_dev_id >= static_cast<int16_t>(nof_gpus)) {
    std::fprintf(stderr,
                 "[gpu_dpdk_mempool] gpu_dev_id=%d out of range (DPDK sees %u GPU(s))\n",
                 cfg.gpu_dev_id,
                 nof_gpus);
    return nullptr;
  }

  const std::size_t total_bytes = static_cast<std::size_t>(cfg.nb_mbufs) * cfg.data_room_size;
  void* const       gpu_base    = rte_gpu_mem_alloc(cfg.gpu_dev_id, total_bytes,  4096);
  if (gpu_base == nullptr) {
    std::fprintf(stderr,
                 "[gpu_dpdk_mempool] rte_gpu_mem_alloc(dev=%d, size=%zu) failed: errno=%d (%s)\n",
                 cfg.gpu_dev_id,
                 total_bytes,
                 rte_errno,
                 strerror(rte_errno));
    return nullptr;
  }

  constexpr std::size_t GPU_PAGE_SIZE = 64 * 1024;

  if (int rc = rte_extmem_register(gpu_base, total_bytes,  nullptr,  0, GPU_PAGE_SIZE);
      rc != 0) {
    std::fprintf(stderr,
                 "[gpu_dpdk_mempool] rte_extmem_register failed: rc=%d errno=%d (%s)\n",
                 rc,
                 rte_errno,
                 strerror(rte_errno));
    rte_gpu_mem_free(cfg.gpu_dev_id, gpu_base);
    return nullptr;
  }
  for (uint16_t port_id : cfg.port_ids) {
    if (!rte_eth_dev_is_valid_port(port_id)) {
      std::fprintf(stderr, "[gpu_dpdk_mempool] port_id %u not valid for dma_map\n", port_id);
      rte_extmem_unregister(gpu_base, total_bytes);
      rte_gpu_mem_free(cfg.gpu_dev_id, gpu_base);
      return nullptr;
    }
    rte_eth_dev_info di{};
    if (rte_eth_dev_info_get(port_id, &di) != 0 || di.device == nullptr) {
      std::fprintf(stderr, "[gpu_dpdk_mempool] dev_info_get(port=%u) failed\n", port_id);
      rte_extmem_unregister(gpu_base, total_bytes);
      rte_gpu_mem_free(cfg.gpu_dev_id, gpu_base);
      return nullptr;
    }
    if (int rc = rte_dev_dma_map(di.device, gpu_base, RTE_BAD_IOVA, total_bytes); rc != 0) {
      std::fprintf(stderr,
                   "[gpu_dpdk_mempool] rte_dev_dma_map(port=%u) failed: rc=%d errno=%d (%s)\n",
                   port_id,
                   rc,
                   rte_errno,
                   strerror(rte_errno));

      for (uint16_t prior : cfg.port_ids) {
        if (prior == port_id) {
          break;
        }
        rte_eth_dev_info di_prior{};
        if (rte_eth_dev_info_get(prior, &di_prior) == 0 && di_prior.device != nullptr) {
          rte_dev_dma_unmap(di_prior.device, gpu_base, RTE_BAD_IOVA, total_bytes);
        }
      }
      rte_extmem_unregister(gpu_base, total_bytes);
      rte_gpu_mem_free(cfg.gpu_dev_id, gpu_base);
      return nullptr;
    }
    std::fprintf(stderr,
                 "[gpu_dpdk_mempool] dma_map(port=%u) ok — VRAM=%p len=%zu now visible to NIC IOMMU\n",
                 port_id,
                 gpu_base,
                 total_bytes);
  }

  rte_pktmbuf_extmem extmem;
  extmem.buf_ptr  = gpu_base;
  extmem.buf_iova = RTE_BAD_IOVA;
  extmem.buf_len  = total_bytes;
  extmem.elt_size = cfg.data_room_size;

  rte_mempool* mp = rte_pktmbuf_pool_create_extbuf(cfg.pool_name,
                                                   cfg.nb_mbufs,
                                                    0,
                                                    0,
                                                   cfg.data_room_size,
                                                   cfg.socket_id,
                                                   &extmem,
                                                   1);
  if (mp == nullptr) {
    std::fprintf(stderr,
                 "[gpu_dpdk_mempool] rte_pktmbuf_pool_create_extbuf(name=%s) failed: errno=%d (%s)\n",
                 cfg.pool_name,
                 rte_errno,
                 strerror(rte_errno));
    for (uint16_t port_id : cfg.port_ids) {
      rte_eth_dev_info di{};
      if (rte_eth_dev_info_get(port_id, &di) == 0 && di.device != nullptr) {
        rte_dev_dma_unmap(di.device, gpu_base, RTE_BAD_IOVA, total_bytes);
      }
    }
    rte_extmem_unregister(gpu_base, total_bytes);
    rte_gpu_mem_free(cfg.gpu_dev_id, gpu_base);
    return nullptr;
  }

  std::unique_ptr<gpu_dpdk_mempool> obj{new (std::nothrow) gpu_dpdk_mempool()};
  if (!obj) {
    rte_mempool_free(mp);
    rte_gpu_mem_free(cfg.gpu_dev_id, gpu_base);
    return nullptr;
  }
  obj->cfg          = cfg;
  obj->gpu_mem_base = gpu_base;
  obj->total_size   = total_bytes;
  obj->pool         = mp;
  return obj;
}

gpu_dpdk_mempool::~gpu_dpdk_mempool()
{

  if (pool != nullptr) {
    rte_mempool_free(pool);
    pool = nullptr;
  }
  if (gpu_mem_base != nullptr) {
    for (uint16_t port_id : cfg.port_ids) {
      rte_eth_dev_info di{};
      if (rte_eth_dev_info_get(port_id, &di) == 0 && di.device != nullptr) {
        rte_dev_dma_unmap(di.device, gpu_mem_base, RTE_BAD_IOVA, total_size);
      }
    }
    rte_extmem_unregister(gpu_mem_base, total_size);
    rte_gpu_mem_free(cfg.gpu_dev_id, gpu_mem_base);
    gpu_mem_base = nullptr;
  }
}

}
}
}
