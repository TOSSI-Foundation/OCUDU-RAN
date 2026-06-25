#pragma once

#include "ocudu/ran/s_nssai.h"
#include <vector>

namespace ocudu {
namespace ocucp {

inline bool snssai_is_covered_by(const s_nssai_t& cell_s, const s_nssai_t& ue_s)
{
  if (not(cell_s.sst == ue_s.sst)) {
    return false;
  }
  if (cell_s.sd.is_default()) {
    return true;
  }
  return cell_s.sd == ue_s.sd;
}

inline bool cell_supports_all_ue_slices(const std::vector<s_nssai_t>& cell_slices,
                                           const std::vector<s_nssai_t>& ue_slices)
{
  if (cell_slices.empty()) {
    return true;
  }
  for (const s_nssai_t& ue_s : ue_slices) {
    bool covered = false;
    for (const s_nssai_t& cell_s : cell_slices) {
      if (snssai_is_covered_by(cell_s, ue_s)) {
        covered = true;
        break;
      }
    }
    if (not covered) {
      return false;
    }
  }
  return true;
}

}
}
