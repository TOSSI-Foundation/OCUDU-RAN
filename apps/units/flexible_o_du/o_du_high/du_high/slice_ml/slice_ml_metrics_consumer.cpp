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

#include "slice_ml_metrics_consumer.h"
#include "../metrics/du_metrics.h"

using namespace ocudu;

slice_ml_controller& du_metrics_consumer_slice_ml::controller_for(pci_t pci)
{
  auto it = controllers.find(pci);
  if (it == controllers.end()) {
    auto ctrl = std::make_unique<slice_ml_controller>(cfg, plmn, embb_sst, embb_sd, urllc_sst, urllc_sd);
    if (configurator != nullptr) {
      ctrl->set_configurator(*configurator);
    }
    it = controllers.emplace(pci, std::move(ctrl)).first;
  }
  return *it->second;
}

void du_metrics_consumer_slice_ml::handle_metric(const app_services::metrics_set& metric)
{
  const odu::du_metrics_report& report = static_cast<const du_metrics_impl&>(metric).get_report();

  if (!report.mac) {
    return;
  }

  for (const scheduler_cell_metrics& cell : report.mac->sched.cells) {
    controller_for(cell.pci).handle_report(cell);
  }
}
