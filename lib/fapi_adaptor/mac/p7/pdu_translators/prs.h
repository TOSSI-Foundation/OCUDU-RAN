// SPDX-FileCopyrightText: Copyright (C) 2021-2026 Software Radio Systems Limited
// SPDX-License-Identifier: BSD-3-Clause-Open-MPI
// Portions of this file may implement 3GPP specifications, which may be subject to additional licensing requirements.

#pragma once

#include "ocudu/fapi/p7/builders/dl_tti_request_builder.h"
#include "ocudu/scheduler/result/prs_info.h"

namespace ocudu {
namespace fapi_adaptor {

class precoding_matrix_mapper;

/// \brief Converts a scheduled DL-PRS into a FAPI PRS PDU.
///
/// The PHY reads the PDU's precoding (one wideband PRG); PRS has no beam of its own here, so it gets the same
/// omnidirectional precoding as a PDSCH without CSI report.
void convert_prs_mac_to_fapi(fapi::dl_tti_request_builder& builder,
                             const prs_info&               prs,
                             const precoding_matrix_mapper& pm_mapper,
                             unsigned                       cell_nof_prbs);

} // namespace fapi_adaptor
} // namespace ocudu
