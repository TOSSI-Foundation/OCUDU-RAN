#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

namespace ocudu::mac_phy_handoff_timing {

enum class msg_kind : uint8_t {
  DL_TTI_REQUEST = 0,
  UL_TTI_REQUEST = 1,
  UL_DCI_REQUEST = 2,
  TX_DATA_REQUEST = 3
};

enum class direction : uint8_t {
  TX_L2 = 0,
  RX_L1 = 1
};

void initialize(bool enabled, const std::string& output_path, std::size_t capacity = 2 * 1024 * 1024);

bool is_enabled();

void record(msg_kind kind, direction dir, uint16_t sfn, uint16_t slot);

void dump_to_csv();

void shutdown();

} // namespace ocudu::mac_phy_handoff_timing
