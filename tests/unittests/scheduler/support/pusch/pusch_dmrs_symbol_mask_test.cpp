// SPDX-FileCopyrightText: Copyright (C) 2021-2026 Software Radio Systems Limited
// SPDX-License-Identifier: BSD-3-Clause-Open-MPI
// Portions of this file may implement 3GPP specifications, which may be subject to additional licensing requirements.

#include "lib/scheduler/support/pusch/pusch_dmrs_symbol_mask.h"
#include "ocudu/adt/static_vector.h"
#include "fmt/ostream.h"
#include <gtest/gtest.h>
#include <map>

using namespace ocudu;

namespace ocudu {

std::ostream& operator<<(std::ostream& os, dmrs_typeA_position dmrs_pos)
{
  switch (dmrs_pos) {
    case dmrs_typeA_position::pos2:
      return os << "pos2";
    case dmrs_typeA_position::pos3:
    default:
      return os << "pos3";
  }
}

std::ostream& operator<<(std::ostream& os, dmrs_additional_positions additional_pos)
{
  switch (additional_pos) {
    case dmrs_additional_positions::pos0:
      return os << "addpos0";
    case dmrs_additional_positions::pos1:
      return os << "addpos1";
    case dmrs_additional_positions::pos2:
      return os << "addpos2";
    case dmrs_additional_positions::pos3:
    default:
      return os << "addpos3";
  }
}

std::ostream& operator<<(std::ostream& os, dmrs_symbol_mask mask)
{
  fmt::print(os, "{}", mask);
  return os;
}

} // namespace ocudu

namespace {
#define L0 (UINT8_MAX)

using PuschDmrsSymbolMaskParams = std::tuple<dmrs_typeA_position, unsigned, dmrs_additional_positions>;

class PuschDmrsSymbolMaskFixture : public ::testing::TestWithParam<PuschDmrsSymbolMaskParams>
{};

const std::map<std::tuple<unsigned, dmrs_additional_positions>, static_vector<unsigned, 4>>
    pusch_dmrs_symbol_mask_typeA_single_table = {
        {{4, dmrs_additional_positions::pos0}, {L0}},         {{4, dmrs_additional_positions::pos1}, {L0}},
        {{4, dmrs_additional_positions::pos2}, {L0}},         {{4, dmrs_additional_positions::pos3}, {L0}},
        {{5, dmrs_additional_positions::pos0}, {L0}},         {{5, dmrs_additional_positions::pos1}, {L0}},
        {{5, dmrs_additional_positions::pos2}, {L0}},         {{5, dmrs_additional_positions::pos3}, {L0}},
        {{6, dmrs_additional_positions::pos0}, {L0}},         {{6, dmrs_additional_positions::pos1}, {L0}},
        {{6, dmrs_additional_positions::pos2}, {L0}},         {{6, dmrs_additional_positions::pos3}, {L0}},
        {{7, dmrs_additional_positions::pos0}, {L0}},         {{7, dmrs_additional_positions::pos1}, {L0}},
        {{7, dmrs_additional_positions::pos2}, {L0}},         {{7, dmrs_additional_positions::pos3}, {L0}},
        {{8, dmrs_additional_positions::pos0}, {L0}},         {{8, dmrs_additional_positions::pos1}, {L0, 7}},
        {{8, dmrs_additional_positions::pos2}, {L0, 7}},      {{8, dmrs_additional_positions::pos3}, {L0, 7}},
        {{9, dmrs_additional_positions::pos0}, {L0}},         {{9, dmrs_additional_positions::pos1}, {L0, 7}},
        {{9, dmrs_additional_positions::pos2}, {L0, 7}},      {{9, dmrs_additional_positions::pos3}, {L0, 7}},
        {{10, dmrs_additional_positions::pos0}, {L0}},        {{10, dmrs_additional_positions::pos1}, {L0, 9}},
        {{10, dmrs_additional_positions::pos2}, {L0, 6, 9}},  {{10, dmrs_additional_positions::pos3}, {L0, 6, 9}},
        {{11, dmrs_additional_positions::pos0}, {L0}},        {{11, dmrs_additional_positions::pos1}, {L0, 9}},
        {{11, dmrs_additional_positions::pos2}, {L0, 6, 9}},  {{11, dmrs_additional_positions::pos3}, {L0, 6, 9}},
        {{12, dmrs_additional_positions::pos0}, {L0}},        {{12, dmrs_additional_positions::pos1}, {L0, 9}},
        {{12, dmrs_additional_positions::pos2}, {L0, 6, 9}},  {{12, dmrs_additional_positions::pos3}, {L0, 5, 8, 11}},
        {{13, dmrs_additional_positions::pos0}, {L0}},        {{13, dmrs_additional_positions::pos1}, {L0, 11}},
        {{13, dmrs_additional_positions::pos2}, {L0, 7, 11}}, {{13, dmrs_additional_positions::pos3}, {L0, 5, 8, 11}},
        {{14, dmrs_additional_positions::pos0}, {L0}},        {{14, dmrs_additional_positions::pos1}, {L0, 11}},
        {{14, dmrs_additional_positions::pos2}, {L0, 7, 11}}, {{14, dmrs_additional_positions::pos3}, {L0, 5, 8, 11}}};

TEST_P(PuschDmrsSymbolMaskFixture, ANormal)
{
  // Extract parameters.
  dmrs_typeA_position             typeA_pos           = std::get<0>(GetParam());
  bounded_integer<uint8_t, 1, 14> last_symbol         = std::get<1>(GetParam());
  dmrs_additional_positions       additional_position = std::get<2>(GetParam());

  // Prepare configuration.
  pusch_dmrs_symbol_mask_mapping_type_A_single_configuration config;
  config.typeA_pos           = typeA_pos;
  config.last_symbol         = last_symbol.value();
  config.additional_position = additional_position;

  // Get mask.
  dmrs_symbol_mask mask = pusch_dmrs_symbol_mask_mapping_type_A_single_get(config);

  // Get expected symbol position list from table.
  ASSERT_TRUE(pusch_dmrs_symbol_mask_typeA_single_table.count({last_symbol.value(), additional_position}));
  static_vector<unsigned, 4> symbol_pos_list =
      pusch_dmrs_symbol_mask_typeA_single_table.at({last_symbol.value(), additional_position});

  // Convert list to mask.
  dmrs_symbol_mask expected(14);
  for (unsigned symbol_index : symbol_pos_list) {
    if (symbol_index == L0) {
      symbol_index = to_symbol_index(typeA_pos);
    }
    expected.set(symbol_index);
  }

  // Assert each mask with the expected.
  ASSERT_EQ(mask, expected);
}

INSTANTIATE_TEST_SUITE_P(PuschDmrsSymbolMaskTypeASingle,
                         PuschDmrsSymbolMaskFixture,
                         ::testing::Combine(::testing::Values(dmrs_typeA_position::pos2, dmrs_typeA_position::pos3),
                                            ::testing::Range(5U, 15U),
                                            ::testing::Values(dmrs_additional_positions::pos0,
                                                              dmrs_additional_positions::pos1,
                                                              dmrs_additional_positions::pos2,
                                                              dmrs_additional_positions::pos3)));

// TS 38.211 Table 6.4.1.1.3-3
using PuschDmrsSymbolMaskTypeBParams = std::tuple<unsigned, unsigned, dmrs_additional_positions>;

class PuschDmrsSymbolMaskTypeBFixture : public ::testing::TestWithParam<PuschDmrsSymbolMaskTypeBParams>
{};

const std::map<std::tuple<unsigned, dmrs_additional_positions>, static_vector<unsigned, 4>>
    pusch_dmrs_symbol_mask_typeB_single_table = {
        {{1, dmrs_additional_positions::pos0}, {0}},          {{1, dmrs_additional_positions::pos1}, {0}},
        {{1, dmrs_additional_positions::pos2}, {0}},          {{1, dmrs_additional_positions::pos3}, {0}},
        {{2, dmrs_additional_positions::pos0}, {0}},          {{2, dmrs_additional_positions::pos1}, {0}},
        {{2, dmrs_additional_positions::pos2}, {0}},          {{2, dmrs_additional_positions::pos3}, {0}},
        {{3, dmrs_additional_positions::pos0}, {0}},          {{3, dmrs_additional_positions::pos1}, {0}},
        {{3, dmrs_additional_positions::pos2}, {0}},          {{3, dmrs_additional_positions::pos3}, {0}},
        {{4, dmrs_additional_positions::pos0}, {0}},          {{4, dmrs_additional_positions::pos1}, {0}},
        {{4, dmrs_additional_positions::pos2}, {0}},          {{4, dmrs_additional_positions::pos3}, {0}},
        {{5, dmrs_additional_positions::pos0}, {0}},          {{5, dmrs_additional_positions::pos1}, {0, 4}},
        {{5, dmrs_additional_positions::pos2}, {0, 4}},       {{5, dmrs_additional_positions::pos3}, {0, 4}},
        {{6, dmrs_additional_positions::pos0}, {0}},          {{6, dmrs_additional_positions::pos1}, {0, 4}},
        {{6, dmrs_additional_positions::pos2}, {0, 4}},       {{6, dmrs_additional_positions::pos3}, {0, 4}},
        {{7, dmrs_additional_positions::pos0}, {0}},          {{7, dmrs_additional_positions::pos1}, {0, 4}},
        {{7, dmrs_additional_positions::pos2}, {0, 4}},       {{7, dmrs_additional_positions::pos3}, {0, 4}},
        {{8, dmrs_additional_positions::pos0}, {0}},          {{8, dmrs_additional_positions::pos1}, {0, 6}},
        {{8, dmrs_additional_positions::pos2}, {0, 3, 6}},    {{8, dmrs_additional_positions::pos3}, {0, 3, 6}},
        {{9, dmrs_additional_positions::pos0}, {0}},          {{9, dmrs_additional_positions::pos1}, {0, 6}},
        {{9, dmrs_additional_positions::pos2}, {0, 3, 6}},    {{9, dmrs_additional_positions::pos3}, {0, 3, 6}},
        {{10, dmrs_additional_positions::pos0}, {0}},         {{10, dmrs_additional_positions::pos1}, {0, 8}},
        {{10, dmrs_additional_positions::pos2}, {0, 4, 8}},   {{10, dmrs_additional_positions::pos3}, {0, 3, 6, 9}},
        {{11, dmrs_additional_positions::pos0}, {0}},         {{11, dmrs_additional_positions::pos1}, {0, 8}},
        {{11, dmrs_additional_positions::pos2}, {0, 4, 8}},   {{11, dmrs_additional_positions::pos3}, {0, 3, 6, 9}},
        {{12, dmrs_additional_positions::pos0}, {0}},         {{12, dmrs_additional_positions::pos1}, {0, 10}},
        {{12, dmrs_additional_positions::pos2}, {0, 5, 10}},  {{12, dmrs_additional_positions::pos3}, {0, 3, 6, 9}},
        {{13, dmrs_additional_positions::pos0}, {0}},         {{13, dmrs_additional_positions::pos1}, {0, 10}},
        {{13, dmrs_additional_positions::pos2}, {0, 5, 10}},  {{13, dmrs_additional_positions::pos3}, {0, 3, 6, 9}},
        {{14, dmrs_additional_positions::pos0}, {0}},         {{14, dmrs_additional_positions::pos1}, {0, 10}},
        {{14, dmrs_additional_positions::pos2}, {0, 5, 10}},  {{14, dmrs_additional_positions::pos3}, {0, 3, 6, 9}}};

TEST_P(PuschDmrsSymbolMaskTypeBFixture, BNormal)
{
  unsigned                  start_symbol        = std::get<0>(GetParam());
  unsigned                  l_d                 = std::get<1>(GetParam());
  dmrs_additional_positions additional_position = std::get<2>(GetParam());

  // TS 38.214 Table 6.1.2.1-1
  if (start_symbol + l_d > 14) {
    GTEST_SKIP();
  }

  pusch_dmrs_symbol_mask_mapping_type_B_single_configuration config;
  config.start_symbol        = start_symbol;
  config.duration            = l_d;
  config.additional_position = additional_position;

  dmrs_symbol_mask mask = pusch_dmrs_symbol_mask_mapping_type_B_single_get(config);

  ASSERT_TRUE(pusch_dmrs_symbol_mask_typeB_single_table.count({l_d, additional_position}));
  static_vector<unsigned, 4> offset_list = pusch_dmrs_symbol_mask_typeB_single_table.at({l_d, additional_position});

  dmrs_symbol_mask expected(14);
  for (unsigned offset : offset_list) {
    expected.set(start_symbol + offset);
  }

  ASSERT_EQ(mask, expected);
}

INSTANTIATE_TEST_SUITE_P(PuschDmrsSymbolMaskTypeBSingle,
                         PuschDmrsSymbolMaskTypeBFixture,
                         ::testing::Combine(::testing::Values(0U, 4U, 10U),
                                            ::testing::Range(1U, 15U),
                                            ::testing::Values(dmrs_additional_positions::pos0,
                                                              dmrs_additional_positions::pos1,
                                                              dmrs_additional_positions::pos2,
                                                              dmrs_additional_positions::pos3)));

// TS 38.214 Table 6.1.2.1-1
TEST(PuschDmrsSymbolMaskTypeBBounds, OutOfSlotAllocationAsserts)
{
  pusch_dmrs_symbol_mask_mapping_type_B_single_configuration config;
  config.start_symbol        = 13;
  config.duration            = 2;
  config.additional_position = dmrs_additional_positions::pos0;

#if ASSERTS_ENABLED
  ASSERT_DEATH({ pusch_dmrs_symbol_mask_mapping_type_B_single_get(config); },
               R"(PUSCH mapping type B allocation S=13 L=2 does not fit in the slot)");
#endif // ASSERTS_ENABLED
}

} // namespace
