// SPDX-FileCopyrightText: Copyright (C) 2021-2026 Software Radio Systems Limited
// SPDX-License-Identifier: BSD-3-Clause-Open-MPI
// Portions of this file may implement 3GPP specifications, which may be subject to additional licensing requirements.

#include "du_low_appconfig_yaml_writer.h"
#include "apps/helpers/logger/logger_appconfig_yaml_writer.h"
#include "apps/helpers/tracing/tracer_appconfig_yaml_writer.h"
#include "apps/services/app_execution_metrics/executor_metrics_config_yaml_writer.h"
#include "apps/services/app_resource_usage/app_resource_usage_config_yaml_writer.h"
#include "apps/services/metrics/metrics_config_yaml_writer.h"
#include "du_low_appconfig.h"

using namespace ocudu;

static void fill_du_appconfig_expert_execution_section(YAML::Node node, const expert_execution_appconfig& config)
{
  {
    YAML::Node affinities_node = node["affinities"];

    if (config.affinities.main_pool_cpu_cfg.mask.any()) {
      affinities_node["main_pool_cpus"] =
          fmt::format("{:,}", span<const size_t>(config.affinities.main_pool_cpu_cfg.mask.get_cpu_ids()));
    }
    affinities_node["main_pool_pinning"] = to_string(config.affinities.main_pool_cpu_cfg.pinning_policy);
  }

  {
    YAML::Node threads_node   = node["threads"];
    YAML::Node main_pool_node = threads_node["main_pool"];
    if (config.threads.main_pool.nof_threads.has_value()) {
      main_pool_node["nof_threads"] = config.threads.main_pool.nof_threads.value();
    } else {
      main_pool_node["nof_threads"] = "auto";
    }
    main_pool_node["task_queue_size"] = config.threads.main_pool.task_queue_size;
    main_pool_node["backoff_period"]  = config.threads.main_pool.backoff_period;
  }
}

static void fill_du_appconfig_remote_control_section(YAML::Node node, const remote_control_appconfig& config)
{
  node["enabled"]   = config.enabled;
  node["bind_addr"] = config.bind_addr;
  node["port"]      = config.port;
}

static void fill_du_appconfig_fapi_stats_section(YAML::Node node, const odu_low::fapi_stats_appconfig& config)
{
  node["enabled"]       = config.enabled;
  node["output_path"]   = config.output_path;
  node["add_timestamp"] = config.add_timestamp;
}

static void fill_du_appconfig_fapi_split_l1_section(YAML::Node node, const odu_low::fapi_split_l1_appconfig& config)
{
  node["rx_cpu"]          = config.rx_cpu;
  node["rx_priority"]     = config.rx_priority;
  node["xsm_device_name"] = config.xsm_device_name;
  node["dpdk_proc_type"]  = config.dpdk_proc_type;
  node["xsm_pair_index"]  = config.xsm_pair_index;
  node["xsm_num_pairs"]   = config.xsm_num_pairs;
}

void ocudu::fill_du_low_appconfig_in_yaml_schema(YAML::Node& node, const du_low_appconfig& config)
{
  app_services::fill_app_resource_usage_config_in_yaml_schema(node, config.metrics_cfg.rusage_config);
  app_services::fill_metrics_appconfig_in_yaml_schema(node, config.metrics_cfg.metrics_service_cfg);
  app_services::fill_app_exec_metrics_config_in_yaml_schema(node, config.metrics_cfg.executors_metrics_cfg);
  fill_logger_appconfig_in_yaml_schema(node, config.log_cfg);
  fill_tracer_appconfig_in_yaml_schema(node, config.trace_cfg);
  fill_du_appconfig_expert_execution_section(node["expert_execution"], config.expert_execution_cfg);
  fill_du_appconfig_remote_control_section(node["remote_control"], config.remote_control_config);
  fill_du_appconfig_fapi_stats_section(node["fapi_stats"], config.fapi_stats_cfg);
  fill_du_appconfig_fapi_split_l1_section(node["fapi_split_l1"], config.fapi_split_l1_cfg);
}
