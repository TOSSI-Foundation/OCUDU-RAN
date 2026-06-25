#include "lib/cu_cp/cell_meas_manager/slice_aware_matching.h"
#include "ocudu/ran/s_nssai.h"
#include <gtest/gtest.h>

using namespace ocudu;
using namespace ocucp;

namespace {

s_nssai_t snssai(uint8_t sst)
{
  return s_nssai_t{slice_service_type{sst}, slice_differentiator{}};
}

s_nssai_t snssai(uint8_t sst, uint32_t sd)
{
  return s_nssai_t{slice_service_type{sst}, slice_differentiator::create(sd).value()};
}

}

TEST(slice_aware_matching, same_sst_no_sd_on_both_is_covered)
{
  EXPECT_TRUE(snssai_is_covered_by(snssai(1), snssai(1)));
}

TEST(slice_aware_matching, different_sst_is_not_covered)
{
  EXPECT_FALSE(snssai_is_covered_by(snssai(2), snssai(1)));
}

TEST(slice_aware_matching, exact_sst_and_sd_match_is_covered)
{
  EXPECT_TRUE(snssai_is_covered_by(snssai(1, 0x1), snssai(1, 0x1)));
}

TEST(slice_aware_matching, sd_wildcard_cell_without_sd_covers_any_sd_of_same_sst)
{
  EXPECT_TRUE(snssai_is_covered_by(snssai(1), snssai(1, 0x1)));
  EXPECT_TRUE(snssai_is_covered_by(snssai(1), snssai(1, 0xabc)));
}

TEST(slice_aware_matching, specific_sd_does_not_cover_different_sd)
{
  EXPECT_FALSE(snssai_is_covered_by(snssai(1, 0x2), snssai(1, 0x1)));
}

TEST(slice_aware_matching, empty_cell_list_supports_all_scenario5_trap4)
{
  EXPECT_TRUE(cell_supports_all_ue_slices({}, {snssai(1)}));
  EXPECT_TRUE(cell_supports_all_ue_slices({}, {snssai(1), snssai(2, 0x3)}));
}

TEST(slice_aware_matching, no_drb_ue_has_no_slice_constraint_scenario7_trap1)
{
  EXPECT_TRUE(cell_supports_all_ue_slices({snssai(2)}, {}));
  EXPECT_TRUE(cell_supports_all_ue_slices({}, {}));
}

TEST(slice_aware_matching, single_slice_ue_supported_only_when_cell_lists_it_scenario2)
{
  EXPECT_FALSE(cell_supports_all_ue_slices({snssai(2)}, {snssai(1)}));
  EXPECT_TRUE(cell_supports_all_ue_slices({snssai(1)}, {snssai(1)}));
}

TEST(slice_aware_matching, multi_slice_ue_requires_all_slices_scenario4)
{
  const std::vector<s_nssai_t> ue_slices = {snssai(1), snssai(2)};
  EXPECT_TRUE(cell_supports_all_ue_slices({snssai(1), snssai(2)}, ue_slices));
  EXPECT_TRUE(cell_supports_all_ue_slices({snssai(2), snssai(1), snssai(3)}, ue_slices));
  EXPECT_FALSE(cell_supports_all_ue_slices({snssai(1)}, ue_slices));
  EXPECT_FALSE(cell_supports_all_ue_slices({snssai(2)}, ue_slices));
}

TEST(slice_aware_matching, sd_wildcard_applies_at_whole_cell_level)
{
  EXPECT_TRUE(cell_supports_all_ue_slices({snssai(1)}, {snssai(1, 0x1)}));
  EXPECT_FALSE(cell_supports_all_ue_slices({snssai(1, 0x2)}, {snssai(1, 0x1)}));
}
