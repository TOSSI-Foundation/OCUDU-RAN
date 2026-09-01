// SPDX-FileCopyrightText: Copyright (C) 2021-2026 Software Radio Systems Limited
// SPDX-License-Identifier: BSD-3-Clause-Open-MPI

#pragma once

#include "ocudu/radio/radio_configuration.h"

namespace ocudu {

class radio_config_rfsimulator_validator : public radio_configuration::validator
{
public:

  bool is_configuration_valid(const radio_configuration::radio& config) const override;
};

}
