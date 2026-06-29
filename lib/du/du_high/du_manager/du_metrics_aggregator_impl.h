// SPDX-FileCopyrightText: Copyright (C) 2021-2026 Software Radio Systems Limited
// SPDX-License-Identifier: BSD-3-Clause-Open-MPI
// Portions of this file may implement 3GPP specifications, which may be subject to additional licensing requirements.

#pragma once

#include "ocudu/du/du_high/du_manager/du_manager.h"
#include "ocudu/du/du_high/du_manager/du_manager_params.h"
#include "ocudu/du/du_high/du_metrics_notifier.h"
#include "ocudu/ran/du_types.h"
#include <functional>

namespace ocudu {

class mac_metrics_notifier;
class scheduler_metrics_notifier;
struct scheduler_metrics_report;

namespace odu {

class du_manager_metrics_aggregator_impl final : public du_manager_mac_metric_aggregator
{
public:
  du_manager_metrics_aggregator_impl(const du_manager_params::metrics_config_params& params_,
                                     task_executor&                                  du_mng_exec_,
                                     timer_manager&                                  timers_,
                                     f1ap_metrics_collector&                         f1ap_collector_);
  ~du_manager_metrics_aggregator_impl() override;

  // DU metrics collector interface
  void aggregate_mac_metrics_report(const mac_metric_report& report) override;

  void handle_cell_start(du_cell_index_t cell_index);
  void handle_cell_stop(du_cell_index_t cell_index);

  void set_bsr_periodicity_recommendation_handler(std::function<void(du_ue_index_t, unsigned)> handler)
  {
    bsr_periodicity_handler = std::move(handler);
  }

private:
  void trigger_report();

  const du_manager_params::metrics_config_params params;
  task_executor&                                 du_mng_exec;
  timer_manager&                                 timers;
  f1ap_metrics_collector&                        f1ap_collector;

  unsigned          next_version = 0;
  du_metrics_report next_report{};

  std::function<void(du_ue_index_t, unsigned)> bsr_periodicity_handler;
};

} // namespace odu
} // namespace ocudu
