// SPDX-FileCopyrightText: Copyright (C) 2021-2026 Software Radio Systems Limited
// SPDX-License-Identifier: BSD-3-Clause-Open-MPI
// Portions of this file may implement 3GPP specifications, which may be subject to additional licensing requirements.

#include "du_low_appconfig_validators.h"
#include "apps/helpers/logger/logger_appconfig_validator.h"
#include "du_low_appconfig.h"
#include "fmt/format.h"

using namespace ocudu;

bool ocudu::validate_du_low_appconfig(const du_low_appconfig& config)
{
  if (!validate_logger_appconfig(config.log_cfg)) {
    return false;
  }

  const auto& l1 = config.fapi_split_l1_cfg;
  if (l1.rx_priority < 1 || l1.rx_priority > 99) {
    fmt::print(stderr, "Error: fapi_split_l1.rx_priority={} is out of range [1, 99]\n", l1.rx_priority);
    return false;
  }
  if (l1.rx_cpu < -1) {
    fmt::print(stderr, "Error: fapi_split_l1.rx_cpu={} is invalid; use -1 (no pinning) or a CPU index >=0\n",
               l1.rx_cpu);
    return false;
  }
  if (l1.xsm_device_name.empty()) {
    fmt::print(stderr, "Error: fapi_split_l1.xsm_device_name must be non-empty\n");
    return false;
  }
  if (l1.dpdk_proc_type != "primary" && l1.dpdk_proc_type != "secondary") {
    fmt::print(stderr,
               "Error: fapi_split_l1.dpdk_proc_type=\"{}\" is invalid; use \"primary\" or \"secondary\"\n",
               l1.dpdk_proc_type);
    return false;
  }

  return true;
}
