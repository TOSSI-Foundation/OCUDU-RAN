// SPDX-FileCopyrightText: Copyright (C) 2021-2026 Software Radio Systems Limited
// SPDX-License-Identifier: BSD-3-Clause-Open-MPI
// Portions of this file may implement 3GPP specifications, which may be subject to additional licensing requirements.

#include "pdsch_dmrs_symbol_mask.h"
#include "ocudu/ran/frame_types.h"
#include "ocudu/support/ocudu_assert.h"
#include <array>

using namespace ocudu;

dmrs_symbol_mask ocudu::pdsch_dmrs_symbol_mask_mapping_type_A_single_get(
    const pdsch_dmrs_symbol_mask_mapping_type_A_single_configuration& config)
{
  unsigned l0 = static_cast<unsigned>(config.typeA_pos);
  unsigned l1 =
      (config.lte_crs_match_around && (config.additional_position == dmrs_additional_positions::pos1 && l0 == 3) &&
       config.ue_capable_additional_dmrs_dl_alt)
          ? 12
          : 11;

  dmrs_symbol_mask mask(14);
  mask.set(l0);

  if (config.last_symbol < 8 || config.additional_position == dmrs_additional_positions::pos0) {
    return mask;
  }

  if (config.last_symbol < 10) {
    mask.set(7);
    return mask;
  }

  if (config.additional_position == dmrs_additional_positions::pos1) {
    if (config.last_symbol < 13) {
      mask.set(9);
      return mask;
    }
    mask.set(l1);
    return mask;
  }

  if (config.additional_position == dmrs_additional_positions::pos2) {
    if (config.last_symbol < 13) {
      mask.set(6);
      mask.set(9);
      return mask;
    }

    mask.set(7);
    mask.set(11);
    return mask;
  }

  if (config.last_symbol < 12) {
    mask.set(6);
    mask.set(9);
    return mask;
  }

  mask.set(5);
  mask.set(8);
  mask.set(11);

  return mask;
}

namespace {

// TS 38.211 Table 7.4.1.1.2-3, Table 6.4.1.1.3-3
struct type_B_additional_positions {
  std::array<uint8_t, 3> l;
};

constexpr std::array<std::array<type_B_additional_positions, 4>, 14> type_B_table = {{
    {{{{0, 0, 0}}, {{0, 0, 0}}, {{0, 0, 0}}, {{0, 0, 0}}}},
    {{{{0, 0, 0}}, {{0, 0, 0}}, {{0, 0, 0}}, {{0, 0, 0}}}},
    {{{{0, 0, 0}}, {{0, 0, 0}}, {{0, 0, 0}}, {{0, 0, 0}}}},
    {{{{0, 0, 0}}, {{0, 0, 0}}, {{0, 0, 0}}, {{0, 0, 0}}}},
    {{{{0, 0, 0}}, {{0, 0, 0}}, {{0, 0, 0}}, {{0, 0, 0}}}},
    {{{{0, 0, 0}}, {{4, 0, 0}}, {{4, 0, 0}}, {{4, 0, 0}}}},
    {{{{0, 0, 0}}, {{4, 0, 0}}, {{4, 0, 0}}, {{4, 0, 0}}}},
    {{{{0, 0, 0}}, {{4, 0, 0}}, {{4, 0, 0}}, {{4, 0, 0}}}},
    {{{{0, 0, 0}}, {{6, 0, 0}}, {{3, 6, 0}}, {{3, 6, 0}}}},
    {{{{0, 0, 0}}, {{7, 0, 0}}, {{4, 7, 0}}, {{4, 7, 0}}}},
    {{{{0, 0, 0}}, {{7, 0, 0}}, {{4, 7, 0}}, {{4, 7, 0}}}},
    {{{{0, 0, 0}}, {{8, 0, 0}}, {{4, 8, 0}}, {{3, 6, 9}}}},
    {{{{0, 0, 0}}, {{9, 0, 0}}, {{5, 9, 0}}, {{3, 6, 9}}}},
    {{{{0, 0, 0}}, {{9, 0, 0}}, {{5, 9, 0}}, {{3, 6, 9}}}},
}};

} // namespace

dmrs_symbol_mask ocudu::pdsch_dmrs_symbol_mask_mapping_type_B_single_get(
    const pdsch_dmrs_symbol_mask_mapping_type_B_single_configuration& config)
{
  const unsigned start = config.start_symbol.value();
  const unsigned l_d   = config.duration.value();

  // TS 38.214 Table 5.1.2.1-1
  ocudu_assert(l_d == 2 or l_d == 4 or l_d == 7,
               "PDSCH mapping type B duration l_d={} is not one of the legal values {{2, 4, 7}} for normal cyclic "
               "prefix (TS38.214 Table 5.1.2.1-1)",
               l_d);
  ocudu_assert(start + l_d <= NOF_OFDM_SYM_PER_SLOT_NORMAL_CP,
               "PDSCH mapping type B allocation S={} L={} does not fit in the slot",
               start,
               l_d);

  dmrs_symbol_mask mask(NOF_OFDM_SYM_PER_SLOT_NORMAL_CP);

  // TS 38.211 Section 7.4.1.1.2
  mask.set(start);

  const type_B_additional_positions& row = type_B_table[l_d][static_cast<unsigned>(config.additional_position)];
  for (uint8_t l : row.l) {
    if (l == 0) {
      break;
    }
    ocudu_assert(l < l_d, "DM-RS position l={} lies outside a PDSCH of duration l_d={}", l, l_d);
    mask.set(start + l);
  }

  return mask;
}
