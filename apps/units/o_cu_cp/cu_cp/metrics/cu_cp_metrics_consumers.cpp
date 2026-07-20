// SPDX-FileCopyrightText: Copyright (C) 2021-2026 Software Radio Systems Limited
// SPDX-License-Identifier: BSD-3-Clause-Open-MPI
// Portions of this file may implement 3GPP specifications, which may be subject to additional licensing requirements.

#include "cu_cp_metrics_consumers.h"
#include "apps/helpers/metrics/json_generators/cu_cp/cu_cp_json_helper.h"
#include "apps/helpers/metrics/json_generators/generator_helpers.h"
#include "apps/services/remote_control/remote_server_metrics_gateway.h"
#include "apps/units/o_cu_cp/cu_cp/cu_cp_unit_config.h"
#include "cu_cp_metrics.h"
#include "ocudu/e2/e2_cu.h"
#include "ocudu/ran/band_helper.h"
#include <algorithm>

using namespace ocudu;

static nlohmann::json build_neighbours_json(const cu_cp_unit_mobility_config& cfg)
{
  nlohmann::json out = nlohmann::json::array();
  for (const auto& cell : cfg.cells) {
    if (cell.ncells.empty()) {
      continue;
    }
    nlohmann::json entry;
    entry["serving_nci"]   = cell.nr_cell_id;
    nlohmann::json& ncells = entry["ncells"] = nlohmann::json::array();
    for (const auto& ncell : cell.ncells) {
      nlohmann::json nc;
      nc["nci"] = ncell.nr_cell_id;
      auto it   = std::find_if(cfg.cells.begin(), cfg.cells.end(), [&ncell](const cu_cp_unit_cell_config_item& c) {
        return c.nr_cell_id == ncell.nr_cell_id;
      });
      if (it != cfg.cells.end()) {
        if (it->pci.has_value()) {
          nc["pci"] = *it->pci;
        }
        if (it->plmn_id.has_value()) {
          nc["plmn"] = *it->plmn_id;
        }
        if (it->tac.has_value()) {
          nc["tac"] = *it->tac;
        }
        if (it->band.has_value()) {
          nc["band"] = nr_band_to_uint(*it->band);
        }
      }
      ncells.push_back(std::move(nc));
    }
    out.push_back(std::move(entry));
  }
  return out;
}

cu_cp_metrics_consumer_json::cu_cp_metrics_consumer_json(app_services::remote_server_metrics_gateway& gateway_,
                                                         const cu_cp_unit_mobility_config&            mobility_cfg) :
  gateway(gateway_), neighbours(build_neighbours_json(mobility_cfg))
{
}

void cu_cp_metrics_consumer_json::handle_metric(const app_services::metrics_set& metric)
{
  const cu_cp_metrics_report& cp_metrics = static_cast<const cu_cp_metrics_impl&>(metric).get_metrics();

  // Only log if there is data.
  if (cp_metrics.dus.empty() && cp_metrics.ngaps.empty()) {
    return;
  }

  nlohmann::json json = app_helpers::json_generators::generate(cp_metrics);
  if (!neighbours.empty()) {
    json["neighbours"] = neighbours;
  }
  gateway.send(json.dump(DEFAULT_JSON_INDENT));
}

void cu_cp_metrics_consumer_log::handle_metric(const app_services::metrics_set& metric)
{
  const cu_cp_metrics_report& cp_metrics = static_cast<const cu_cp_metrics_impl&>(metric).get_metrics();

  ngap_consumer.handle_metric(cp_metrics.ngaps, cp_metrics.mobility);
  rrc_consumer.handle_metric(cp_metrics.dus, cp_metrics.mobility);
}

void cu_cp_metrics_consumer_e2::handle_metric(const app_services::metrics_set& metric)
{
  notifier.report_metrics(static_cast<const cu_cp_metrics_impl&>(metric).get_metrics());
}
