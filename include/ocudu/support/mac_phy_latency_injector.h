#pragma once

#include <cstdint>

namespace ocudu::mac_phy_latency_injector {

enum class msg_kind : uint8_t {
  DL_TTI_REQUEST  = 0,
  UL_TTI_REQUEST  = 1,
  UL_DCI_REQUEST  = 2,
  TX_DATA_REQUEST = 3
};

void initialize_from_env();

void initialize(bool     enabled,
                uint64_t dl_tti_mean_ns,
                uint64_t ul_tti_mean_ns,
                uint64_t ul_dci_mean_ns,
                uint64_t tx_data_mean_ns,
                uint32_t jitter_pct);

bool is_enabled();

void inject(msg_kind kind);

void shutdown();

} // namespace ocudu::mac_phy_latency_injector
