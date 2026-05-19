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
namespace odu {

struct f1ap_appconfig {
  std::vector<std::string> cu_cp_addresses = {"127.0.10.1"};
  std::vector<std::string> bind_addresses = {"127.0.10.2"};
};

struct f1u_appconfig {
  unsigned              pdu_queue_size = 2048;
  f1u_sockets_appconfig f1u_sockets;
};

struct metrics_appconfig {
  app_services::app_resource_usage_config rusage_config;
  app_services::metrics_appconfig         metrics_service_cfg;
  app_services::executor_metrics_config   executors_metrics_cfg;
  bool                                    autostart_stdout_metrics = false;
};

struct fapi_stats_appconfig {
  bool        enabled       = false;
  std::string output_path   = "./logs/odu_fapi_stats.json";
  bool        add_timestamp = true;
};

struct fapi_split_l2_appconfig {
  int rx_cpu      = -1;
  int rx_priority = 80;
  /// xSM memzone name (standalone: "xsm_0"; XFAPI bridge: configured device).
  std::string xsm_device_name = "xsm_0";
  /// Slot-pair index inside the shared xSM memzone.
  unsigned xsm_pair_index = 0;
  /// DPDK --file-prefix for EAL init.
  std::string xsm_file_prefix = "gnb0";
  /// DPDK proc-type ("primary" or "secondary") for EAL init.
    std::string dpdk_proc_type = "secondary";
};

} // namespace odu

struct du_appconfig {
  du_appconfig() { log_cfg.filename = "/tmp/du.log"; }
  logger_appconfig log_cfg;
  tracer_appconfig trace_cfg;
  odu::metrics_appconfig metrics_cfg;
  odu::f1ap_appconfig f1ap_cfg;
  odu::f1u_appconfig f1u_cfg;
  app_services::buffer_pool_appconfig buffer_pool_config;
  expert_execution_appconfig expert_execution_cfg;
  std::optional<hal_appconfig> hal_config;
  remote_control_appconfig remote_control_config;
  odu::fapi_stats_appconfig fapi_stats_cfg;
  odu::fapi_split_l2_appconfig fapi_split_l2_cfg;
  bool enable_dryrun = false;
};

} // namespace ocudu
