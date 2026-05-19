// SPDX-FileCopyrightText: Copyright (C) 2021-2026 Software Radio Systems Limited
// SPDX-License-Identifier: BSD-3-Clause-Open-MPI
// Portions of this file may implement 3GPP specifications, which may be subject to additional licensing requirements.

#include "du_low_appconfig_cli11_schema.h"
#include "apps/helpers/hal/hal_cli11_schema.h"
#include "apps/helpers/logger/logger_appconfig_cli11_schema.h"
#include "apps/helpers/tracing/tracer_appconfig_cli11_schema.h"
#include "apps/services/app_execution_metrics/executor_metrics_config_cli11_schema.h"
#include "apps/services/app_resource_usage/app_resource_usage_config_cli11_schema.h"
#include "apps/services/metrics/metrics_config_cli11_schema.h"
#include "apps/services/remote_control/remote_control_appconfig_cli11_schema.h"
#include "apps/services/worker_manager/worker_manager_cli11_schema.h"
#include "du_low_appconfig.h"

using namespace ocudu;

static void configure_cli11_with_fapi_split_l1_schema(CLI::App& app, odu_low::fapi_split_l1_appconfig& cfg)
{
  CLI::App* sub =
      app.add_subcommand("fapi_split_l1", "FAPI-split L1 (odu_low) transport scheduling")->configurable();
  sub->add_option("--rx_cpu", cfg.rx_cpu,
                  "CPU to pin the xSM RX thread to (>=0); -1 = no pinning")
      ->capture_default_str();
  sub->add_option("--rx_priority", cfg.rx_priority,
                  "SCHED_FIFO priority for the xSM RX thread (default 80)")
      ->capture_default_str();
  sub->add_option("--xsm_device_name", cfg.xsm_device_name,
                  "xSM memzone name. Default \"xsm_0\" for standalone deployment; "
                  "set to e.g. \"xsm_l1side\" when running with the XFAPI bridge")
      ->capture_default_str();
  sub->add_option("--dpdk_proc_type", cfg.dpdk_proc_type,
                  "DPDK process type: \"primary\" (standalone, owns the runtime) or "
                  "\"secondary\" (XFAPI bridge, attaches to XFAPI's primary)")
      ->capture_default_str()
      ->check(CLI::IsMember({"primary", "secondary"}));
  sub->add_option("--xsm_pair_index", cfg.xsm_pair_index,
                  "xSM slot-pair index inside the shared memzone. 0 for both "
                  "standalone and XFAPI bridge (odu_low always sits on pair 0)")
      ->capture_default_str();
  sub->add_option("--xsm_num_pairs", cfg.xsm_num_pairs,
                  "Number of slot pairs reserved when odu_low creates the xSM "
                  "memzone (slave side only). 1 = standalone, 2 = XFAPI bridge")
      ->capture_default_str()
      ->check(CLI::Range(1u, 2u));
}

static void configure_cli11_with_fapi_stats_schema(CLI::App& app, odu_low::fapi_stats_appconfig& cfg)
{
  // ->configurable() required so CLI11 parses the YAML block (not just the flag).
  CLI::App* sub =
      app.add_subcommand("fapi_stats", "FAPI message-stats recorder configuration")->configurable();
  sub->add_option("--enabled", cfg.enabled,
                  "Record every FAPI message to RAM and dump to JSON on shutdown "
                  "(~832 MiB RAM cost)")
      ->capture_default_str();
  sub->add_option("--output_path", cfg.output_path,
                  "Output JSON path (parent dir auto-created; absolute path recommended)")
      ->capture_default_str();
  sub->add_option("--add_timestamp", cfg.add_timestamp,
                  "If true, insert _YYYYMMDD_HHMMSS before the .json extension so "
                  "each run gets a unique file")
      ->capture_default_str();
}

void ocudu::configure_cli11_with_du_low_appconfig_schema(CLI::App& app, du_low_appconfig& config)
{
  app.add_flag("--dryrun", config.enable_dryrun, "Enable application dry run mode")->capture_default_str();

  // Loggers section.
  configure_cli11_with_logger_appconfig_schema(app, config.log_cfg);

  configure_cli11_with_fapi_stats_schema(app, config.fapi_stats_cfg);

  configure_cli11_with_fapi_split_l1_schema(app, config.fapi_split_l1_cfg);

  // Tracers section.
  configure_cli11_with_tracer_appconfig_schema(app, config.trace_cfg);

  // Expert execution section.
  configure_cli11_with_worker_manager_appconfig_schema(app, config.expert_execution_cfg);

  // Metrics section.
  app_services::configure_cli11_with_executor_metrics_appconfig_schema(app, config.metrics_cfg.executors_metrics_cfg);
  app_services::configure_cli11_with_app_resource_usage_config_schema(app, config.metrics_cfg.rusage_config);
  app_services::configure_cli11_with_metrics_appconfig_schema(app, config.metrics_cfg.metrics_service_cfg);

  // Remote control section.
  configure_cli11_with_remote_control_appconfig_schema(app, config.remote_control_config);

#ifdef DPDK_FOUND
  // HAL section.
  config.hal_config.emplace();
  configure_cli11_with_hal_appconfig_schema(app, *config.hal_config);
#else
  app.failure_message([](const CLI::App* application, const CLI::Error& e) -> std::string {
    if (std::string(e.what()).find("INI was not able to parse hal.++") == std::string::npos) {
      return CLI::FailureMessage::simple(application, e);
    }

    return "Invalid configuration detected, 'hal' section is present but the application was built without DPDK "
           "support\n" +
           CLI::FailureMessage::simple(application, e);
  });
#endif
}

#ifdef DPDK_FOUND
static void manage_hal_optional(CLI::App& app, du_low_appconfig& gnb_cfg)
{
  if (!is_hal_section_present(app)) {
    gnb_cfg.hal_config.reset();
  }
}
#endif

void ocudu::autoderive_du_low_parameters_after_parsing(CLI::App& app, du_low_appconfig& config)
{
#ifdef DPDK_FOUND
  manage_hal_optional(app, config);
#endif
}
