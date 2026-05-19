#pragma once

#include "split6_plugin.h"
#include <memory>

namespace ocudu {

class split6_plugin_xsm : public split6_plugin
{
public:
  explicit split6_plugin_xsm(std::string_view app_name) {}

  void on_parsing_configuration_registration(CLI::App& app) override {}

  bool on_configuration_validation() const override { return true; }

  void on_loggers_registration() override {}

  void fill_worker_manager_config(worker_manager_config& config) override {}

  std::unique_ptr<fapi_adaptor::phy_fapi_adaptor>
  create_fapi_adaptor(const odu::du_high_configuration& du_high_cfg,
                      const o_du_unit_dependencies&     dependencies) override;
};

} // namespace ocudu
