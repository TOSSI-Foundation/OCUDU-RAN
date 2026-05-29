// SPDX-FileCopyrightText: Copyright (C) 2021-2026 Software Radio Systems Limited
// SPDX-License-Identifier: BSD-3-Clause-Open-MPI

#pragma once

#include "ocudu/support/units.h"
#include <atomic>
#include <cstdint>
#include <memory>
#include <string>
#include <thread>
#include <vector>

struct rte_flow;
struct rte_mempool;

namespace ocudu {

namespace hal {
namespace cuda {

class inline_prach_pipeline;

class gpu_dpdk_mempool;
}
}

namespace ether {

/// DPDK configuration settings.
constexpr unsigned MAX_BURST_SIZE  = 64;
constexpr unsigned MAX_BUFFER_SIZE = 9600;

struct dpdk_gpu_rx_config {
  bool                  enabled    = false;
  int                   gpu_dev_id = 0;
  std::vector<uint16_t> prach_eaxcs;

  std::shared_ptr<hal::cuda::inline_prach_pipeline> inline_pipeline;

  unsigned iq_payload_offset_bytes = 37;
};

/// DPDK port configuration.
struct dpdk_port_config {
  /// Device identifier.
  std::string id;
  /// Promiscuous mode flag.
  bool is_promiscuous_mode_enabled;
  /// Ethernet link status checking flag.
  bool is_link_status_check_enabled;
  /// MTU size.
  units::bytes mtu_size;
  dpdk_gpu_rx_config gpu_rx;
};

/// \brief DPDK Ethernet port context.
///
/// Encapsulates and manages the lifetime of the internal DPDK resources of an Ethernet port.
class dpdk_port_context
{
  dpdk_port_context(const std::string& port_id_, unsigned dpdk_port_id_, ::rte_mempool* mem_pool_) :
    port_id(port_id_), dpdk_port_id(dpdk_port_id_), mem_pool(mem_pool_)
  {
  }

public:
  /// Creates and initializes a new DPDK port context with the given configuration.
  static std::shared_ptr<dpdk_port_context> create(const dpdk_port_config& config);

  ~dpdk_port_context();

  /// Returns the DPDK port identifier of this context .
  unsigned get_dpdk_port_id() const { return dpdk_port_id; }

  /// Returns the string port identifier of this context.
  const std::string& get_port_id() const { return port_id; }

  /// Returns the mbuf memory pool of this context.
  ::rte_mempool* get_mempool() { return mem_pool; }

  /// Returns the mbuf memory pool of this context.
  const ::rte_mempool* get_mempool() const { return mem_pool; }

  const dpdk_gpu_rx_config& get_gpu_rx_config() const { return gpu_rx; }

  uint64_t get_gpu_prach_frame_count() const { return gpu_prach_frames.load(std::memory_order_relaxed); }
  uint64_t get_gpu_other_frame_count() const { return gpu_other_frames.load(std::memory_order_relaxed); }

private:
  const std::string    port_id;
  const unsigned       dpdk_port_id;
  ::rte_mempool* const mem_pool;

  dpdk_gpu_rx_config                          gpu_rx;
  std::shared_ptr<hal::cuda::gpu_dpdk_mempool> gpu_mempool;
  std::vector<::rte_flow*>                    gpu_flow_rules;
  std::thread                                 gpu_listener_thread;
  std::atomic<bool>                           gpu_stop{false};
  std::atomic<uint64_t>                       gpu_prach_frames{0};
  std::atomic<uint64_t>                       gpu_other_frames{0};

  friend bool init_port_with_gpu(dpdk_port_context& ctx, const dpdk_port_config& config, unsigned port_id);
};

} // namespace ether
} // namespace ocudu
