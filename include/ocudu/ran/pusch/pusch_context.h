// SPDX-FileCopyrightText: Copyright (C) 2021-2026 Software Radio Systems Limited
// SPDX-License-Identifier: BSD-3-Clause-Open-MPI
// Portions of this file may implement 3GPP specifications, which may be subject to additional licensing requirements.

#pragma once

#include "ocudu/ran/harq_id.h"
#include "ocudu/ran/rnti.h"

namespace ocudu {

/// PUSCH transmission context.
class pusch_context
{
public:
  /// Default constructor.
  pusch_context() = default;

  /// Constructs from values.
  explicit pusch_context(rnti_t rnti_, harq_id_t h_id_) : rnti(rnti_), h_id(h_id_) {}

  rnti_t get_rnti() const { return rnti; }
  harq_id_t get_h_id() const { return h_id; }

private:
  friend struct fmt::formatter<pusch_context>;
  rnti_t    rnti = rnti_t::INVALID_RNTI;
  harq_id_t h_id = INVALID_HARQ_ID;
};

} // namespace ocudu
