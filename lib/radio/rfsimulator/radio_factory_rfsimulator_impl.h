// SPDX-FileCopyrightText: Copyright (C) 2021-2026 Software Radio Systems Limited
// SPDX-License-Identifier: BSD-3-Clause-Open-MPI

#pragma once

#include "ocudu/radio/radio_factory.h"

namespace ocudu {

class radio_factory_rfsimulator_impl : public radio_factory
{
public:

  const radio_configuration::validator& get_configuration_validator() const override;

  std::unique_ptr<radio_session> create(const radio_configuration::radio& config,
                                        task_executor&                    async_task_executor,
                                        radio_event_notifier&             notifier) override;
};

}
