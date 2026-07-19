
#pragma once

#include "ocudu/radio/radio_factory.h"

namespace ocudu {
class radio_factory_sionna_impl : public radio_factory
{
public:
  const radio_configuration::validator& get_configuration_validator() const override;

  std::unique_ptr<radio_session> create(const radio_configuration::radio& config,
                                        task_executor&                    async_task_executor,
                                        radio_event_notifier&             notifier) override;
};

} // namespace ocudu
