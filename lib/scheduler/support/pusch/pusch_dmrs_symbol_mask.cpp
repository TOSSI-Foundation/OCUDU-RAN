// SPDX-FileCopyrightText: Copyright (C) 2021-2026 Software Radio Systems Limited
// SPDX-License-Identifier: BSD-3-Clause-Open-MPI
// Portions of this file may implement 3GPP specifications, which may be subject to additional licensing requirements.

#include "pusch_dmrs_symbol_mask.h"
#include "ocudu/ran/frame_types.h"
#include "ocudu/support/ocudu_assert.h"
#include <array>

using namespace ocudu;

dmrs_symbol_mask ocudu::pusch_dmrs_symbol_mask_mapping_type_A_single_get(
    const pusch_dmrs_symbol_mask_mapping_type_A_single_configuration& config)
{
  unsigned l0 = static_cast<unsigned>(config.typeA_pos);

  dmrs_symbol_mask mask(14);
  mask.set(l0);

  if (config.last_symbol < 8 || config.additional_position == dmrs_additional_positions::pos0) {
    return mask;
  }

  if (config.last_symbol < 10) {
    mask.set(7);
    return mask;
  }

  if (config.last_symbol < 13 &&
      (config.last_symbol != 12 || config.additional_position < dmrs_additional_positions::pos3)) {
    mask.set(9);
    if (config.additional_position >= dmrs_additional_positions::pos2) {
      mask.set(6);
    }
    return mask;
  }

  mask.set(11);
  if (config.additional_position == dmrs_additional_positions::pos2) {
    mask.set(7);
  } else if (config.additional_position == dmrs_additional_positions::pos3) {
    mask.set(5);
    mask.set(8);
  }

  return mask;
}

namespace {

// TS 38.211 Table 6.4.1.1.3-3
struct type_B_additional_positions {
  std::array<uint8_t, 3> l;
};

static constexpr std::array<std::array<type_B_additional_positions, 4>, 15> type_B_table = {{
    {{{{0, 0, 0}}, {{0, 0, 0}}, {{0, 0, 0}}, {{0, 0, 0}}}},
    {{{{0, 0, 0}}, {{0, 0, 0}}, {{0, 0, 0}}, {{0, 0, 0}}}},
    {{{{0, 0, 0}}, {{0, 0, 0}}, {{0, 0, 0}}, {{0, 0, 0}}}},
    {{{{0, 0, 0}}, {{0, 0, 0}}, {{0, 0, 0}}, {{0, 0, 0}}}},
    {{{{0, 0, 0}}, {{0, 0, 0}}, {{0, 0, 0}}, {{0, 0, 0}}}},
    {{{{0, 0, 0}}, {{4, 0, 0}}, {{4, 0, 0}}, {{4, 0, 0}}}},
    {{{{0, 0, 0}}, {{4, 0, 0}}, {{4, 0, 0}}, {{4, 0, 0}}}},
    {{{{0, 0, 0}}, {{4, 0, 0}}, {{4, 0, 0}}, {{4, 0, 0}}}},
    {{{{0, 0, 0}}, {{6, 0, 0}}, {{3, 6, 0}}, {{3, 6, 0}}}},
    {{{{0, 0, 0}}, {{6, 0, 0}}, {{3, 6, 0}}, {{3, 6, 0}}}},
    {{{{0, 0, 0}}, {{8, 0, 0}}, {{4, 8, 0}}, {{3, 6, 9}}}},
    {{{{0, 0, 0}}, {{8, 0, 0}}, {{4, 8, 0}}, {{3, 6, 9}}}},
    {{{{0, 0, 0}}, {{10, 0, 0}}, {{5, 10, 0}}, {{3, 6, 9}}}},
    {{{{0, 0, 0}}, {{10, 0, 0}}, {{5, 10, 0}}, {{3, 6, 9}}}},
    {{{{0, 0, 0}}, {{10, 0, 0}}, {{5, 10, 0}}, {{3, 6, 9}}}},
}};

} // namespace

dmrs_symbol_mask ocudu::pusch_dmrs_symbol_mask_mapping_type_B_single_get(
    const pusch_dmrs_symbol_mask_mapping_type_B_single_configuration& config)
{
  const unsigned start = config.start_symbol.value();
  const unsigned l_d   = config.duration.value();

  // TS 38.214 Table 6.1.2.1-1, Section 6.1.2.1
  ocudu_assert(start + l_d <= NOF_OFDM_SYM_PER_SLOT_NORMAL_CP,
               "PUSCH mapping type B allocation S={} L={} does not fit in the slot",
               start,
               l_d);

  dmrs_symbol_mask mask(NOF_OFDM_SYM_PER_SLOT_NORMAL_CP);

  // TS 38.211 Section 6.4.1.1.3
  mask.set(start);

  const type_B_additional_positions& row = type_B_table[l_d][static_cast<unsigned>(config.additional_position)];
  for (uint8_t l : row.l) {
    if (l == 0) {
      break;
    }
    ocudu_assert(l < l_d, "DM-RS position l={} lies outside a PUSCH of duration l_d={}", l, l_d);
    mask.set(start + l);
  }

  return mask;
}
