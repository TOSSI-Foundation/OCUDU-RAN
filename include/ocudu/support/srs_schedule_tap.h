#pragma once

#include "ocudu/ran/srs/srs_resource_configuration.h"
#include <array>
#include <atomic>
#include <cstdint>

namespace ocudu {
namespace srs_schedule_tap {

inline constexpr std::size_t RING_SIZE = 64;

struct slot_entry {

  std::atomic<uint32_t> packed_slot_id{0xFFFFFFFFu};
  std::atomic<uint8_t>  start_symbol{0xFFu};
  std::atomic<uint8_t>  nof_symbols{0};

  srs_resource_configuration resource{};
  uint8_t                    nof_rx_ports{0};
};

inline std::array<slot_entry, RING_SIZE> slots{};

inline void publish(uint32_t                          packed_slot_id,
                    uint8_t                           start_sym,
                    uint8_t                           nof_sym,
                    const srs_resource_configuration& resource,
                    uint8_t                           nof_rx_ports)
{
  auto& s = slots[packed_slot_id & (RING_SIZE - 1)];
  s.start_symbol.store(start_sym, std::memory_order_relaxed);
  s.nof_symbols.store(nof_sym, std::memory_order_relaxed);
  s.resource     = resource;
  s.nof_rx_ports = nof_rx_ports;
  s.packed_slot_id.store(packed_slot_id, std::memory_order_release);
}

inline bool is_srs_symbol(uint32_t packed_slot_id, uint8_t symbol_idx)
{
  const auto& s = slots[packed_slot_id & (RING_SIZE - 1)];
  if (s.packed_slot_id.load(std::memory_order_acquire) != packed_slot_id) {
    return false;
  }
  const uint8_t start = s.start_symbol.load(std::memory_order_relaxed);
  const uint8_t nof   = s.nof_symbols.load(std::memory_order_relaxed);
  if (start == 0xFFu) {
    return false;
  }
  return symbol_idx >= start && symbol_idx < static_cast<uint8_t>(start + nof);
}

inline bool lookup_resource(uint32_t packed_slot_id, srs_resource_configuration& out_resource, uint8_t& out_nof_rx)
{
  const auto& s = slots[packed_slot_id & (RING_SIZE - 1)];
  if (s.packed_slot_id.load(std::memory_order_acquire) != packed_slot_id) {
    return false;
  }
  out_resource = s.resource;
  out_nof_rx   = s.nof_rx_ports;
  return true;
}

}
}
