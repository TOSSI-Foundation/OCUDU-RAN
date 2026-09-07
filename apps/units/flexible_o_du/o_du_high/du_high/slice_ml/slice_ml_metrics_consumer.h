// Copyright 2025-2026 coRAN LABS Private Limited
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#pragma once

#include "apps/services/metrics/metrics_consumer.h"
#include "slice_ml_controller.h"
#include <map>
#include <memory>

namespace ocudu {

class du_metrics_consumer_slice_ml : public app_services::metrics_consumer
{
public:
  du_metrics_consumer_slice_ml(const slice_ml_expert_config& cfg_,
                               plmn_identity                 plmn_,
                               uint8_t                       embb_sst_,
                               uint32_t                      embb_sd_,
                               uint8_t                       urllc_sst_,
                               uint32_t                      urllc_sd_) :
    cfg(cfg_), plmn(plmn_), embb_sst(embb_sst_), embb_sd(embb_sd_), urllc_sst(urllc_sst_), urllc_sd(urllc_sd_)
  {
  }

  void set_configurator(odu::du_configurator& cfgr)
  {
    configurator = &cfgr;
    for (auto& c : controllers) {
      c.second->set_configurator(cfgr);
    }
  }

  void handle_metric(const app_services::metrics_set& metric) override;

private:
  slice_ml_controller& controller_for(pci_t pci);

  slice_ml_expert_config cfg;
  plmn_identity          plmn;
  uint8_t                embb_sst;
  uint32_t               embb_sd;
  uint8_t                urllc_sst;
  uint32_t               urllc_sd;
  odu::du_configurator*  configurator = nullptr;

  std::map<pci_t, std::unique_ptr<slice_ml_controller>> controllers;
};

}
