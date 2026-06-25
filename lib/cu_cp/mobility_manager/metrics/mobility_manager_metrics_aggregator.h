// SPDX-FileCopyrightText: Copyright (C) 2021-2026 Software Radio Systems Limited
// SPDX-License-Identifier: BSD-3-Clause-Open-MPI
// Portions of this file may implement 3GPP specifications, which may be subject to additional licensing requirements.

#pragma once

#include "ocudu/cu_cp/mobility_management_metrics.h"

namespace ocudu::ocucp {

class mobility_manager_metrics_aggregator
{
public:
  /// \brief Aggregates the metrics for the requested handover preparation.
  void aggregate_requested_handover_preparation();

  /// \brief Aggregates the metrics for the successful handover preparation.
  void aggregate_successful_handover_preparation();

  /// \brief Aggregates the metrics for the requested handover execution.
  void aggregate_requested_handover_execution();

  /// \brief Aggregates the metrics for the successful handover execution.
  void aggregate_successful_handover_execution();

  void aggregate_suppressed_handover_slice_mismatch();

  mobility_management_metrics request_metrics_report() const;

private:
  mobility_management_metrics aggregated_mobility_manager_metrics;
};

} // namespace ocudu::ocucp
