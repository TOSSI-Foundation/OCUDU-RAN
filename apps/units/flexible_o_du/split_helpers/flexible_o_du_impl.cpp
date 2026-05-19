// SPDX-FileCopyrightText: Copyright (C) 2021-2026 Software Radio Systems Limited
// SPDX-License-Identifier: BSD-3-Clause-Open-MPI
// Portions of this file may implement 3GPP specifications, which may be subject to additional licensing requirements.

#include "flexible_o_du_impl.h"
#include "ocudu/du/du_low/du_low.h"
#include "ocudu/du/du_low/o_du_low.h"
#include "ocudu/du/o_du.h"
#include "ocudu/phy/upper/upper_phy.h"
#include "ocudu/ru/ru.h"
#include "ocudu/ru/ru_controller.h"
#include "ocudu/support/fapi_split_trace.h"

using namespace ocudu;

flexible_o_du_impl::flexible_o_du_impl(unsigned nof_cells_, flexible_o_du_metrics_notifier* notifier) :
  nof_cells(nof_cells_),
  ru_ul_adapt(nof_cells_),
  ru_timing_adapt(nof_cells_),
  ru_error_adapt(nof_cells_),
  odu_metrics_handler(notifier)
{
}

void flexible_o_du_impl::start()
{
  fapi_split_trace::event("RU_SETUP", "DU-CU monolithic: starting ODU-DU operation controller");
  du->get_operation_controller().start();
  fapi_split_trace::event("RU_SETUP", "DU-CU monolithic: starting RU operation controller (OFH/DPDK/NIC bring-up)");
  ru->get_controller().get_operation_controller().start();
  fapi_split_trace::event("RU_SETUP", "DU-CU monolithic: RU operation controller START returned — L1<->RU path live");
}

void flexible_o_du_impl::stop()
{
  fapi_split_trace::event("RU_SETUP", "DU-CU monolithic: stop requested");
  ru->get_controller().get_operation_controller().stop();
  fapi_split_trace::event("RU_SETUP", "DU-CU monolithic: RU controller stopped");
  du->get_operation_controller().stop();
  fapi_split_trace::event("RU_SETUP", "DU-CU monolithic: ODU-DU stopped");
}

void flexible_o_du_impl::add_ru(std::unique_ptr<radio_unit> active_ru)
{
  ru = std::move(active_ru);
  ocudu_assert(ru, "Invalid Radio Unit");

  // Connect the RU adaptor to the RU.
  ru_dl_rg_adapt.connect(ru->get_downlink_plane_handler());
  fapi_split_trace::event("RU_SETUP",
                          "DU-CU monolithic: ru_dl_rg_adapter connected to RU downlink_plane_handler");
  ru_ul_request_adapt.connect(ru->get_uplink_plane_handler());
  fapi_split_trace::event("RU_SETUP",
                          "DU-CU monolithic: ru_ul_request_adapter connected to RU uplink_plane_handler");

  // Update the RU metrics collector.
  if (auto* collector = ru->get_metrics_collector()) {
    odu_metrics_handler.set_ru_metrics_collector(*collector);
  }
}

void flexible_o_du_impl::add_du(std::unique_ptr<odu::o_du> active_du)
{
  du = std::move(active_du);
  ocudu_assert(du, "Cannot set an invalid DU");

  // Connect all the sectors of the DU low to the RU adaptors.
  for (unsigned i = 0; i != nof_cells; ++i) {
    auto& upper = du->get_o_du_low().get_du_low().get_upper_phy(i);
    // Make connections between DU and RU.
    ru_ul_adapt.map_handler(i, upper.get_rx_symbol_handler());
    fapi_split_trace::event("RU_SETUP",
                            "DU-CU monolithic: sector %u ru_ul_adapt mapped to upper_phy rx_symbol_handler", i);
    ru_timing_adapt.map_handler(i, upper.get_timing_handler());
    fapi_split_trace::event("RU_SETUP",
                            "DU-CU monolithic: sector %u ru_timing_adapt mapped to upper_phy timing_handler", i);
    ru_error_adapt.map_handler(i, upper.get_error_handler());
    fapi_split_trace::event("RU_SETUP",
                            "DU-CU monolithic: sector %u ru_error_adapt mapped to upper_phy error_handler", i);
  }
}
