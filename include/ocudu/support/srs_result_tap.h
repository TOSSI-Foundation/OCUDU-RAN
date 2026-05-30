#pragma once

#include "ocudu/phy/upper/signal_processors/srs/srs_estimator_result.h"
#include <array>
#include <atomic>
#include <cstdint>

namespace ocudu {
namespace srs_result_tap {

inline constexpr std::size_t  RING_SIZE   = 64;
inline constexpr std::uint32_t EMPTY_SLOT = 0xFFFFFFFFu;

struct slot_entry {
  std::atomic<std::uint32_t> ready_slot_id{EMPTY_SLOT};
  srs_estimator_result       result;
};

inline std::array<slot_entry, RING_SIZE> slots{};

inline void publish(std::uint32_t packed_slot_id, const srs_estimator_result& result)
{
  auto& s  = slots[packed_slot_id & (RING_SIZE - 1)];
  s.result = result;
  s.ready_slot_id.store(packed_slot_id, std::memory_order_release);
}

inline bool consume(std::uint32_t packed_slot_id, srs_estimator_result& out)
{
  auto& s = slots[packed_slot_id & (RING_SIZE - 1)];
  if (s.ready_slot_id.load(std::memory_order_acquire) != packed_slot_id) {
    return false;
  }
  out = s.result;
  s.ready_slot_id.store(EMPTY_SLOT, std::memory_order_release);
  return true;
}

}
}
