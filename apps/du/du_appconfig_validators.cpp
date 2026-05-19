// SPDX-FileCopyrightText: Copyright (C) 2021-2026 Software Radio Systems Limited
// SPDX-License-Identifier: BSD-3-Clause-Open-MPI
// Portions of this file may implement 3GPP specifications, which may be subject to additional licensing requirements.

#include "du_appconfig_validators.h"
#include "apps/helpers/f1u/f1u_appconfig_validator.h"
#include "apps/helpers/logger/logger_appconfig_validator.h"

using namespace ocudu;

bool ocudu::validate_appconfig(const du_appconfig& config)
{
  if (!validate_logger_appconfig(config.log_cfg)) {
    return false;
  }

  if (config.f1ap_cfg.cu_cp_addresses.empty()) {
    fmt::print("CU-CP F1-C address is mandatory\n");
    return false;
  }

  for (const auto& addr : config.f1ap_cfg.cu_cp_addresses) {
    if (addr.empty()) {
      fmt::print("CU-CP F1-C address cannot be empty\n");
      return false;
    }
  }

  if (!validate_f1u_sockets_appconfig(config.f1u_cfg.f1u_sockets)) {
    return false;
  }

  const auto& l2 = config.fapi_split_l2_cfg;
  if (l2.rx_priority < 1 || l2.rx_priority > 99) {
    fmt::print("fapi_split_l2.rx_priority={} is out of range [1, 99]\n", l2.rx_priority);
    return false;
  }
  if (l2.rx_cpu < -1) {
    fmt::print("fapi_split_l2.rx_cpu={} is invalid; use -1 (no pinning) or a CPU index >=0\n", l2.rx_cpu);
    return false;
  }
  if (l2.xsm_device_name.empty()) {
    fmt::print("fapi_split_l2.xsm_device_name must be non-empty\n");
    return false;
  }

  return true;
}
