// SPDX-FileCopyrightText: Copyright (C) 2021-2026 Software Radio Systems Limited
// SPDX-License-Identifier: BSD-3-Clause-Open-MPI
// Portions of this file may implement 3GPP specifications, which may be subject to additional licensing requirements.

#pragma once

#include "ocudu/ran/du_types.h"
#include "ocudu/ran/pci.h"
#include "ocudu/ran/rnti.h"
#include "ocudu/ran/s_nssai.h"

namespace ocudu {

/// Adds/Removes UEs from the metrics.
class sched_metrics_ue_configurator
{
public:
  virtual ~sched_metrics_ue_configurator() = default;

  /// Adds a new UE to the reported metrics.
  virtual void handle_ue_creation(du_ue_index_t ue_index, rnti_t rnti, pci_t pcell_pci) = 0;

  /// Handle a reconfiguration of an existing UE.
  virtual void handle_ue_reconfiguration(du_ue_index_t ue_index) = 0;

  /// Update the S-NSSAI (slice) associated with a UE, for per-slice metrics. Default no-op.
  virtual void handle_ue_slice_update(du_ue_index_t ue_index, const s_nssai_t& s_nssai) {}

  /// Removes a UE from the reported metrics.
  virtual void handle_ue_deletion(du_ue_index_t ue_index) = 0;
};

} // namespace ocudu
