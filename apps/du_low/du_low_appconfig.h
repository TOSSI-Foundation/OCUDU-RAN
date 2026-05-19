// SPDX-FileCopyrightText: Copyright (C) 2021-2026 Software Radio Systems Limited
// SPDX-License-Identifier: BSD-3-Clause-Open-MPI
// Portions of this file may implement 3GPP specifications, which may be subject to additional licensing requirements.

#pragma once

#include "apps/helpers/f1u/f1u_appconfig.h"
#include "apps/helpers/hal/hal_appconfig.h"
#include "apps/helpers/logger/logger_appconfig.h"
#include "apps/helpers/tracing/tracer_appconfig.h"
#include "apps/services/app_execution_metrics/executor_metrics_config.h"
#include "apps/services/app_resource_usage/app_resource_usage_config.h"
#include "apps/services/buffer_pool/buffer_pool_appconfig.h"
#include "apps/services/metrics/metrics_appconfig.h"
#include "apps/services/remote_control/remote_control_appconfig.h"
#include "apps/services/worker_manager/worker_manager_appconfig.h"
#include <optional>
#include <string>

namespace ocudu {

namespace odu_low {
/// Metrics report configuration.
struct metrics_appconfig {
  app_services::app_resource_usage_config rusage_config;
  app_services::metrics_appconfig         metrics_service_cfg;
  app_services::executor_metrics_config   executors_metrics_cfg;
};

struct fapi_stats_appconfig {
  bool enabled = false;
  std::string output_path = "./logs/odu_low_fapi_stats.json";
  bool add_timestamp = true;
};

struct fapi_split_l1_appconfig {
  int rx_cpu      = -1;
  int rx_priority = 80;
  /// xSM memzone name.
  std::string xsm_device_name = "xsm_0";
  /// DPDK proc-type ("primary" standalone, "secondary" with XFAPI bridge).
  std::string dpdk_proc_type = "primary";
  unsigned xsm_pair_index = 0;
  /// Number of slot pairs to reserve (1 standalone, 2 with XFAPI bridge).
  unsigned xsm_num_pairs = 1;
};

} // namespace odu_low

struct du_low_appconfig {
  /// Default constructor to update the log filename.
  du_low_appconfig() { log_cfg.filename = "/tmp/du_low.log"; }
  /// Loggers configuration.
  logger_appconfig log_cfg;
  /// Tracers configuration.
  tracer_appconfig trace_cfg;
  /// Metrics configuration.
  odu_low::metrics_appconfig metrics_cfg;
  /// Expert configuration.
  expert_execution_appconfig expert_execution_cfg;
  /// Remote control configuration.
  remote_control_appconfig remote_control_config;
  /// HAL configuration.
  std::optional<hal_appconfig> hal_config;
  odu_low::fapi_stats_appconfig fapi_stats_cfg;
  odu_low::fapi_split_l1_appconfig fapi_split_l1_cfg;
  /// Dryrun mode enabled flag.
  bool enable_dryrun = false;
};

} // namespace ocudu
