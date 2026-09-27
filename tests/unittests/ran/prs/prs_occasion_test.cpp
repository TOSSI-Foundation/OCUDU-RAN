// SPDX-FileCopyrightText: Copyright (C) 2021-2026 Software Radio Systems Limited
// SPDX-License-Identifier: BSD-3-Clause-Open-MPI
// Portions of this file may implement 3GPP specifications, which may be subject to additional licensing requirements.

#include "ocudu/ran/prs/prs.h"
#include <gtest/gtest.h>

using namespace ocudu;

static prs_cell_config make_cfg(unsigned period, unsigned set_offset, unsigned res_offset)
{
  return prs_cell_config{.period_slots         = period,
                         .set_slot_offset      = set_offset,
                         .resource_slot_offset = res_offset,
                         .n_id                 = 0,
                         .comb_size            = prs_comb_size::four,
                         .comb_offset          = 0,
                         .nof_symbols          = prs_num_symbols::four,
                         .start_symbol         = 2,
                         .crbs                 = {0, 48},
                         .tx_power_dbm         = 0,
                         .power_offset_db      = std::nullopt};
}

// TS 38.211 Section 7.4.1.7.4 with T_rep = 1: transmitted iff (N_slot^frame n_f + n_s - T_offset - T_offset,res) mod
// T_per == 0. Checked against a direct evaluation of that formula over two full SFN cycles, so the wrap-around from
// SFN 1023 to 0 is covered.
TEST(prs_occasion_test, matches_ts38211_formula_across_sfn_wrap)
{
  for (subcarrier_spacing scs : {subcarrier_spacing::kHz15, subcarrier_spacing::kHz30}) {
    const unsigned mu               = to_numerology_value(scs);
    const unsigned slots_per_frame  = 10U << mu;
    const unsigned cycle            = 1024 * slots_per_frame;
    const auto     cfg              = make_cfg(160U << mu, 37, 5);
    slot_point     sl{mu, 0};
    for (unsigned i = 0; i != 2 * cycle; ++i, ++sl) {
      const unsigned n_f      = sl.sfn();
      const unsigned n_s      = sl.slot_index();
      const long     lhs      = static_cast<long>(slots_per_frame * n_f + n_s) - 37 - 5;
      const long     per      = 160L << mu;
      const bool     expected = ((lhs % per) + per) % per == 0;
      ASSERT_EQ(prs_slot_is_occasion(cfg, sl), expected) << "scs=" << to_string(scs) << " slot=" << sl.to_uint();
    }
  }
}

TEST(prs_occasion_test, one_occasion_per_period)
{
  const auto cfg   = make_cfg(80, 0, 0);
  unsigned   count = 0;
  slot_point sl{0, 0};
  for (unsigned i = 0; i != 10240; ++i, ++sl) {
    count += prs_slot_is_occasion(cfg, sl) ? 1 : 0;
  }
  EXPECT_EQ(count, 10240U / 80U);
}

TEST(prs_occasion_test, allowed_periods_follow_numerology)
{
  // 15 kHz set, and 2^mu times it for 30 kHz (TS 38.211 Section 7.4.1.7.4).
  EXPECT_TRUE(prs_valid_period(4, subcarrier_spacing::kHz15));
  EXPECT_TRUE(prs_valid_period(10240, subcarrier_spacing::kHz15));
  EXPECT_FALSE(prs_valid_period(128, subcarrier_spacing::kHz15));
  EXPECT_FALSE(prs_valid_period(6, subcarrier_spacing::kHz15));
  EXPECT_TRUE(prs_valid_period(8, subcarrier_spacing::kHz30));
  EXPECT_TRUE(prs_valid_period(20480, subcarrier_spacing::kHz30));
  EXPECT_FALSE(prs_valid_period(4, subcarrier_spacing::kHz30));
}

TEST(prs_occasion_test, valid_symbol_comb_pairs_are_the_ts38211_table)
{
  // {L_PRS, K_comb} of TS 38.211 Section 7.4.1.7.3 (without the Rel-18 L_PRS = 1 entries).
  const std::pair<unsigned, unsigned> allowed[] = {{2, 2}, {4, 2}, {6, 2}, {12, 2}, {4, 4}, {12, 4}, {6, 6}, {12, 6},
                                                   {12, 12}};
  for (unsigned l : {2U, 4U, 6U, 12U}) {
    for (unsigned k : {2U, 4U, 6U, 12U}) {
      const bool expected = std::find(std::begin(allowed), std::end(allowed), std::make_pair(l, k)) != std::end(allowed);
      EXPECT_EQ(prs_valid_num_symbols_and_comb_size(static_cast<prs_num_symbols>(l), static_cast<prs_comb_size>(k)),
                expected)
          << "L=" << l << " K=" << k;
    }
  }
}
