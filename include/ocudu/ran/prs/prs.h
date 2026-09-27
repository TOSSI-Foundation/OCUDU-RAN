// SPDX-FileCopyrightText: Copyright (C) 2021-2026 Software Radio Systems Limited
// SPDX-License-Identifier: BSD-3-Clause-Open-MPI
// Portions of this file may implement 3GPP specifications, which may be subject to additional licensing requirements.

/// \file
/// \brief Positioning Reference Signals (PRS) parameters.

#pragma once

#include "ocudu/ran/resource_allocation/rb_interval.h"
#include "ocudu/ran/slot_point.h"
#include "ocudu/ran/subcarrier_spacing.h"
#include <algorithm>
#include <array>
#include <cstdint>
#include <optional>

namespace ocudu {

//// PRS transmission time domain duration.
enum class prs_num_symbols : uint8_t { two = 2, four = 4, six = 6, twelve = 12 };

/// PRS transmission comb size.
enum class prs_comb_size : uint8_t { two = 2, four = 4, six = 6, twelve = 12 };

/// \brief Determines whether the combination of time domain duration and comb size is valid.
///
/// The valid combinations are given in TS38.211 Section 7.4.1.7.3.
inline bool prs_valid_num_symbols_and_comb_size(prs_num_symbols nsymb, prs_comb_size comb_sz)
{
  uint8_t nsymb_u8   = static_cast<uint8_t>(nsymb);
  uint8_t comb_sz_u8 = static_cast<uint8_t>(comb_sz);
  return (nsymb_u8 >= comb_sz_u8) && (nsymb_u8 % comb_sz_u8 == 0);
}

// \brief Cell-level DL-PRS configuration: one PRS resource set with one PRS resource, on the cell's own
// carrier.
struct prs_cell_config {
  /// Resource set periodicity \f$T_{per}^{PRS}\f$ in slots (dl-PRS-Periodicity-and-ResourceSetSlotOffset).
  unsigned period_slots;
  /// Resource set slot offset \f$T_{offset}^{PRS}\f$ with respect to SFN0 slot0, {0, ..., period_slots - 1}.
  unsigned set_slot_offset;
  /// Resource slot offset \f$T_{offset,res}^{PRS}\f$ within the set, {0, ..., 511} (dl-PRS-ResourceSlotOffset).
  unsigned resource_slot_offset;
  /// Sequence ID \f$n_{ID,seq}^{PRS}\f$, {0, ..., 4095} (dl-PRS-SequenceID).
  unsigned n_id;
  /// Comb size \f$K_{comb}^{PRS}\f$ (dl-PRS-CombSizeN).
  prs_comb_size comb_size;
  /// RE offset \f$k_{offset}^{PRS}\f$, {0, ..., comb_size - 1} (dl-PRS-CombSizeN-AndReOffset).
  unsigned comb_offset;
  /// Number of symbols \f$L_{PRS}\f$ (dl-PRS-NumSymbols).
  prs_num_symbols nof_symbols;
  /// First symbol \f$l_{start}^{PRS}\f$ within the slot, {0, ..., 12} (dl-PRS-ResourceSymbolOffset).
  unsigned start_symbol;
  /// PRBs with respect to Point A (dl-PRS-StartPRB, dl-PRS-ResourceBandwidth): 24 to 272 in steps of 4.
  crb_interval crbs;
  /// EPRE of the PRS REs in dBm, {-60, ..., 50}, as reported to the LMF (dl-PRS-ResourcePower). The gNB does not
  /// derive it; it is the operator's declared value.
  int tx_power_dbm;
  /// Power offset applied by the PHY, in dB relative to its reference (FAPI PRS power offset). Empty for none.
  std::optional<float> power_offset_db;
};

/// Allowed \f$T_{per}^{PRS}\f$ in slots for 15 kHz, TS 38.211 Section 7.4.1.7.4; multiply by \f$2^\mu\f$ for others.
constexpr std::array<unsigned, 17> prs_base_periods_slots = {
    4, 5, 8, 10, 16, 20, 32, 40, 64, 80, 160, 320, 640, 1280, 2560, 5120, 10240};

/// Whether \c period_slots is an allowed PRS resource set periodicity for \c scs (TS 38.211 Section 7.4.1.7.4).
inline bool prs_valid_period(unsigned period_slots, subcarrier_spacing scs)
{
  const unsigned mult = 1U << to_numerology_value(scs);
  return std::any_of(prs_base_periods_slots.begin(), prs_base_periods_slots.end(), [&](unsigned p) {
    return p * mult == period_slots;
  });
}

// \brief Whether the PRS resource is transmitted in \c slot, TS 38.211 Section 7.4.1.7.4:
// \f$(N_{slot}^{frame,\mu} n_f + n_{s,f}^\mu - T_{offset}^{PRS} - T_{offset,res}^{PRS}) \bmod T_{per}^{PRS}
// = 0\f$ (repetition factor 1, no muting).
inline bool prs_slot_is_occasion(const prs_cell_config& cfg, slot_point slot)
{
  const unsigned cycle  = slot.nof_slots_per_hyper_system_frame();
  const unsigned offset = (cfg.set_slot_offset + cfg.resource_slot_offset) % cycle;
  return (slot.to_uint() + cycle - offset) % cfg.period_slots == 0;
}

} // namespace ocudu
