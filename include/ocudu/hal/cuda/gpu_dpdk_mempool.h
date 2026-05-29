#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <vector>

struct rte_mempool;

namespace ocudu {
namespace hal {
namespace cuda {

constexpr unsigned GPU_MEMPOOL_DEFAULT_NB_MBUFS  = 2048;
constexpr unsigned GPU_MEMPOOL_DEFAULT_DATA_ROOM = 4096;
constexpr int      GPU_MEMPOOL_DEFAULT_GPU_ID    = 0;
constexpr int      GPU_MEMPOOL_DEFAULT_SOCKET    = 0;

struct gpu_dpdk_mempool_config {

  int16_t gpu_dev_id = GPU_MEMPOOL_DEFAULT_GPU_ID;

  unsigned nb_mbufs = GPU_MEMPOOL_DEFAULT_NB_MBUFS;

  unsigned data_room_size = GPU_MEMPOOL_DEFAULT_DATA_ROOM;

  int socket_id = GPU_MEMPOOL_DEFAULT_SOCKET;

  const char* pool_name = "ocudu_gpu_prach_pool";

  std::vector<uint16_t> port_ids;
};

class gpu_dpdk_mempool
{
public:

  static std::unique_ptr<gpu_dpdk_mempool> create(const gpu_dpdk_mempool_config& cfg);

  ~gpu_dpdk_mempool();

  gpu_dpdk_mempool(const gpu_dpdk_mempool&)            = delete;
  gpu_dpdk_mempool& operator=(const gpu_dpdk_mempool&) = delete;

  rte_mempool* mempool() const { return pool; }

  void* gpu_buffer_base() const { return gpu_mem_base; }

  std::size_t gpu_buffer_size() const { return total_size; }

  int16_t dpdk_gpu_dev_id() const { return cfg.gpu_dev_id; }

  unsigned data_room_size() const { return cfg.data_room_size; }

private:
  gpu_dpdk_mempool() = default;

  gpu_dpdk_mempool_config cfg{};
  void*                   gpu_mem_base = nullptr;
  std::size_t             total_size   = 0;
  rte_mempool*            pool         = nullptr;
};

}
}
}
