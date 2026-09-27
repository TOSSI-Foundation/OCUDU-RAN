// SPDX-FileCopyrightText: Copyright (C) 2021-2026 Software Radio Systems Limited
// SPDX-License-Identifier: BSD-3-Clause-Open-MPI
// Portions of this file may implement 3GPP specifications, which may be subject to additional licensing requirements.

#pragma once

#include "../cell/resource_grid.h"
#include "../config/cell_configuration.h"
#include "ocudu/ocudulog/logger.h"

namespace ocudu {

// \brief Schedules the cell's DL-PRS (TS 38.211 Section 7.4.1.7) and reserves its REs in the DL resource
// grid.
class prs_scheduler
{
public:
  explicit prs_scheduler(const cell_configuration& cfg_);

  /// Schedule PRS up to max_dl_slot_alloc_delay slots ahead on the first call, then one new slot per call.
  void run_slot(cell_resource_allocator& res_alloc);

  /// Called when the cell is deactivated.
  void stop() { first_run_slot = true; }

private:
  void schedule_prs(cell_slot_resource_allocator& slot_alloc) const;

  const cell_configuration& cell_cfg;
  ocudulog::basic_logger&   logger;
  bool                      first_run_slot = true;
};

} // namespace ocudu
