// SPDX-FileCopyrightText: Copyright (C) 2021-2026 Software Radio Systems Limited
// SPDX-License-Identifier: BSD-3-Clause-Open-MPI
// Portions of this file may implement 3GPP specifications, which may be subject to additional licensing requirements.

#include "prs.h"
#include "ocudu/fapi_adaptor/precoding_matrix_mapper.h"

using namespace ocudu;
using namespace fapi_adaptor;

void ocudu::fapi_adaptor::convert_prs_mac_to_fapi(fapi::dl_tti_request_builder& builder,
                                                  const prs_info&               prs,
                                                  const precoding_matrix_mapper& pm_mapper,
                                                  unsigned                       cell_nof_prbs)
{
  fapi::dl_prs_pdu_builder prs_builder = builder.add_prs_pdu();
  prs_builder.set_bwp_parameters(prs.bwp_cfg->scs, prs.bwp_cfg->cp)
      .set_n_id(prs.n_id)
      .set_symbol_parameters(prs.nof_symbols, prs.start_symbol)
      .set_rb_parameters(prs.crbs)
      .set_comb_parameters(prs.comb_size, prs.comb_offset)
      .set_power_offset(prs.power_offset_db);

  fapi::tx_precoding_and_beamforming_pdu_builder pm_bf_builder = prs_builder.get_tx_precoding_and_beamforming_pdu_builder();
  pm_bf_builder.set_prg_parameters(cell_nof_prbs);
  pm_bf_builder.set_pmi(pm_mapper.map({}, 1));
}
