// SPDX-FileCopyrightText: Copyright (C) 2021-2026 Software Radio Systems Limited
// SPDX-License-Identifier: BSD-3-Clause-Open-MPI
// Portions of this file may implement 3GPP specifications, which may be subject to additional licensing requirements.

#include "prs_scheduler.h"
#include "ocudu/ocudulog/ocudulog.h"

using namespace ocudu;

prs_scheduler::prs_scheduler(const cell_configuration& cfg_) :
  cell_cfg(cfg_), logger(ocudulog::fetch_basic_logger("SCHED"))
{
}

void prs_scheduler::run_slot(cell_resource_allocator& res_alloc)
{
  if (not cell_cfg.params.prs.has_value()) {
    return;
  }
  if (first_run_slot) {
    for (unsigned i = 0; i != res_alloc.max_dl_slot_alloc_delay + 1; ++i) {
      schedule_prs(res_alloc[i]);
    }
    first_run_slot = false;
  } else {
    schedule_prs(res_alloc[res_alloc.max_dl_slot_alloc_delay]);
  }
}

void prs_scheduler::schedule_prs(cell_slot_resource_allocator& slot_alloc) const
{
  const prs_cell_config& prs = *cell_cfg.params.prs;
  const slot_point       sl  = slot_alloc.slot;
  if (not prs_slot_is_occasion(prs, sl)) {
    return;
  }

  const ofdm_symbol_range symbols{prs.start_symbol, prs.start_symbol + static_cast<unsigned>(prs.nof_symbols)};
  // TDD: every PRS symbol must be a DL symbol of this slot.
  if (not cell_cfg.is_dl_enabled(sl) or symbols.stop() > cell_cfg.get_nof_dl_symbol_per_slot(sl)) {
    logger.warning("PRS occasion at slot={} skipped: symbols {} are not all downlink", sl, symbols);
    return;
  }

  const bwp_configuration& bwp = cell_cfg.params.dl_cfg_common.init_dl_bwp.generic_params;
  const grant_info         grant{bwp.scs, symbols, prs.crbs};
  // SSB is scheduled first. TS 38.211 7.4.1.7.3 excludes SSB symbols from the PRS mapping, but a PRS PDU covers a
  // contiguous symbol range, so the configuration validator rejects any PRS occasion that can meet an SSB; landing
  // here means the grid disagrees with that check, and sending PRS over the SSB would corrupt both.
  if (slot_alloc.dl_res_grid.collides(grant)) {
    logger.error("PRS occasion at slot={} skipped: symbols {} x {} are already in use", sl, symbols, prs.crbs);
    return;
  }
  if (slot_alloc.result.dl.prs.full()) {
    logger.error("PRS occasion at slot={} skipped: no room in the DL result", sl);
    return;
  }

  slot_alloc.dl_res_grid.fill(grant);
  slot_alloc.result.dl.prs.push_back(prs_info{.bwp_cfg         = &bwp,
                                              .crbs            = prs.crbs,
                                              .n_id            = prs.n_id,
                                              .comb_size       = prs.comb_size,
                                              .comb_offset     = prs.comb_offset,
                                              .nof_symbols     = prs.nof_symbols,
                                              .start_symbol    = prs.start_symbol,
                                              .power_offset_db = prs.power_offset_db});
}
