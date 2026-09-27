// SPDX-FileCopyrightText: Copyright (C) 2021-2026 Software Radio Systems Limited
// SPDX-License-Identifier: BSD-3-Clause-Open-MPI
// Portions of this file may implement 3GPP specifications, which may be subject to additional licensing requirements.

#pragma once

#include "ocudu/ran/prs/prs.h"
#include "ocudu/scheduler/config/bwp_configuration.h"
#include <optional>

namespace ocudu {

/// DL-PRS transmission in a slot, TS 38.211 Section 7.4.1.7.
struct prs_info {
  const bwp_configuration* bwp_cfg;
  /// PRBs with respect to Point A (CRB0).
  crb_interval crbs;
  /// Sequence ID \f$n_{ID,seq}^{PRS}\f$, {0, ..., 4095}.
  unsigned        n_id;
  prs_comb_size   comb_size;
  unsigned        comb_offset;
  prs_num_symbols nof_symbols;
  unsigned        start_symbol;
  std::optional<float> power_offset_db;
};

} // namespace ocudu
