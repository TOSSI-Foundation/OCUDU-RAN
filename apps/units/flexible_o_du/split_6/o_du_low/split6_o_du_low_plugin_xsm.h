#pragma once

#include "split6_o_du_low_plugin.h"
#include <memory>

namespace ocudu {

class split6_o_du_low_plugin_xsm : public split6_o_du_low_plugin
{
public:
  explicit split6_o_du_low_plugin_xsm(std::string_view app_name) {}

  void on_parsing_configuration_registration(CLI::App& app) override {}

  bool on_configuration_validation() const override { return true; }

  void on_loggers_registration() override {}

  void fill_worker_manager_config(worker_manager_config& config) override {}

  std::unique_ptr<fapi_adaptor::mac_fapi_p5_sector_adaptor>
  create_fapi_p5_sector_adaptor(fapi::p5_requests_gateway& p5_gateway,
                                task_executor&             executor,
                                task_executor&             control_executor) override;

  std::unique_ptr<fapi_adaptor::mac_fapi_p7_sector_adaptor_factory>
  create_fapi_p7_sector_adaptor_factory(task_executor& executor, task_executor& control_executor) override;
};

} // namespace ocudu
