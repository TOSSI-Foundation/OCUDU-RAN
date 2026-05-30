// SPDX-FileCopyrightText: Copyright (C) 2021-2026 Software Radio Systems Limited
// SPDX-License-Identifier: BSD-3-Clause-Open-MPI

#pragma once

#include "ocudu/ofh/ethernet/ethernet_mac_address.h"
#include "ocudu/support/units.h"
#include <cstdint>
#include <memory>
#include <vector>

namespace ocudu {
namespace hal {
namespace cuda {
class inline_prach_pipeline;
class inline_srs_pipeline;
}
}

namespace ether {

/// Configuration for the Ethernet transmitter.
struct transmitter_config {
  /// Ethernet interface name or identifier.
  std::string interface;
  /// Promiscuous mode flag.
  bool is_promiscuous_mode_enabled = false;
  /// Ethernet link status checking flag.
  bool is_link_status_check_enabled = false;
  /// If set to true, metrics are enabled in the Ethernet transmitter.
  bool are_metrics_enabled = false;
  /// MTU size.
  units::bytes mtu_size;
  /// Destination MAC address.
  mac_address mac_dst_address;

  bool                  enable_gpu_rx_queue = false;
  std::vector<uint16_t> gpu_prach_eaxcs;
  std::shared_ptr<hal::cuda::inline_prach_pipeline> inline_pipeline;
  unsigned gpu_iq_payload_offset_bytes = 37;

  bool                  enable_srs_classification = false;
  std::vector<uint16_t> gpu_ul_eaxcs;
  std::shared_ptr<hal::cuda::inline_srs_pipeline> srs_inline_pipeline;
};

} // namespace ether
} // namespace ocudu
