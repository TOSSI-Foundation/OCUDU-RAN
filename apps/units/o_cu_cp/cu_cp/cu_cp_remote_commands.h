// SPDX-FileCopyrightText: Copyright (C) 2021-2026 Software Radio Systems Limited
// SPDX-License-Identifier: BSD-3-Clause-Open-MPI
// Portions of this file may implement 3GPP specifications, which may be subject to additional licensing requirements.

#pragma once

#include "apps/services/remote_control/remote_command.h"
#include "ocudu/cu_cp/cu_cp_command_handler.h"

namespace ocudu {

/// Remote command that triggers a handover of a UE to a target cell.
class handover_remote_command : public app_services::remote_command
{
  ocucp::cu_cp_command_handler& cu_cp;

public:
  explicit handover_remote_command(ocucp::cu_cp_command_handler& cu_cp_) : cu_cp(cu_cp_) {}

  // See interface for documentation.
  std::string_view get_name() const override { return "ho"; }

  // See interface for documentation.
  std::string_view get_description() const override { return "Triggers a handover of a UE to a target cell"; }

  // See interface for documentation.
  error_type<std::string> execute(const nlohmann::json& json) override;
};

} // namespace ocudu
