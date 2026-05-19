// SPDX-FileCopyrightText: Copyright (C) 2021-2026 Software Radio Systems Limited
// SPDX-License-Identifier: BSD-3-Clause-Open-MPI
// Portions of this file may implement 3GPP specifications, which may be subject to additional licensing requirements.

#include "split6_flexible_o_du_low_session.h"
#include "ocudu/du/du_low/du_low.h"
#include "ocudu/du/du_operation_controller.h"
#include "ocudu/fapi_adaptor/mac/p7/mac_fapi_p7_sector_adaptor.h"
#include "ocudu/fapi_adaptor/phy/p7/phy_fapi_p7_sector_fastpath_adaptor.h"
#include "ocudu/fapi_adaptor/phy/phy_fapi_fastpath_adaptor.h"
#include "ocudu/fapi_adaptor/phy/phy_fapi_sector_fastpath_adaptor.h"
#include "ocudu/phy/upper/upper_phy.h"
#include "ocudu/ru/ru_controller.h"
#include "ocudu/support/fapi_split_trace.h"
#include "ocudu/support/ocudu_assert.h"

using namespace ocudu;

split6_flexible_o_du_low_session::~split6_flexible_o_du_low_session()
{
  fapi_split_trace::event("RU_SETUP", "session destructor: stopping RU + ODU-Low");

  // Stop RU.
  ru->get_controller().get_operation_controller().stop();
  fapi_split_trace::event("RU_SETUP", "RU controller stopped");

  // Stop O-DU low.
  odu_low->get_operation_controller().stop();
  fapi_split_trace::event("RU_SETUP", "ODU-Low stopped");
}

void split6_flexible_o_du_low_session::set_dependencies(
    std::unique_ptr<fapi_adaptor::mac_fapi_p7_sector_adaptor> slot_msg_adaptor,
    std::unique_ptr<odu::o_du_low>                            du,
    std::unique_ptr<radio_unit>                               radio,
    unique_timer                                              timer)
{
  ocudu_assert(slot_msg_adaptor, "Invalid FAPI P7 message adaptor");
  ocudu_assert(du, "Invalid O-DU low");
  ocudu_assert(radio, "Invalid Radio Unit");

  mac_p7_adaptor = std::move(slot_msg_adaptor);
  odu_low        = std::move(du);
  ru             = std::move(radio);

  fapi_split_trace::event("RU_SETUP",
                          "set_dependencies: MAC-P7 adaptor + ODU-Low + RU instances moved in (nof_cells=%u)",
                          NOF_CELLS_SUPPORTED);

  // Connect the RU adaptor to the RU.
  ru_dl_rg_adapt.connect(ru->get_downlink_plane_handler());
  fapi_split_trace::event("RU_SETUP",
                          "ru_dl_rg_adapter connected to RU downlink_plane_handler (L1 DL grid -> RU path live)");
  ru_ul_request_adapt.connect(ru->get_uplink_plane_handler());
  fapi_split_trace::event("RU_SETUP",
                          "ru_ul_request_adapter connected to RU uplink_plane_handler (L1 UL request -> RU path live)");

  // Connect all the sectors of the DU low to the RU adaptors.
  for (unsigned i = 0; i != NOF_CELLS_SUPPORTED; ++i) {
    // Make connections between DU and RU.
    auto& upper = odu_low->get_du_low().get_upper_phy(i);
    ru_ul_adapt.map_handler(i, upper.get_rx_symbol_handler());
    fapi_split_trace::event("RU_SETUP", "sector %u: ru_ul_adapt mapped to upper_phy rx_symbol_handler", i);
    ru_timing_adapt.map_handler(i, upper.get_timing_handler());
    fapi_split_trace::event("RU_SETUP", "sector %u: ru_timing_adapt mapped to upper_phy timing_handler", i);
    ru_error_adapt.map_handler(i, upper.get_error_handler());
    fapi_split_trace::event("RU_SETUP", "sector %u: ru_error_adapt mapped to upper_phy error_handler", i);

    // Connect adaptor with O-DU low.
    auto& fapi_adaptor = odu_low->get_phy_fapi_fastpath_adaptor().get_sector_adaptor(i).get_p7_sector_adaptor();
    fapi_adaptor.set_p7_slot_indication_notifier(mac_p7_adaptor->get_p7_slot_indication_notifier());
    fapi_split_trace::event("RU_SETUP",
                            "sector %u: FAPI P7 slot_indication_notifier wired (L1 -> xSM -> L2)", i);
    fapi_adaptor.set_p7_indications_notifier(mac_p7_adaptor->get_p7_indications_notifier());
    fapi_split_trace::event("RU_SETUP",
                            "sector %u: FAPI P7 indications_notifier wired (L1 -> xSM -> L2)", i);
    fapi_adaptor.set_error_indication_notifier(mac_p7_adaptor->get_error_indication_notifier());
    fapi_split_trace::event("RU_SETUP",
                            "sector %u: FAPI error_indication_notifier wired (L1 -> xSM -> L2)", i);
  }

  fapi_split_trace::event("RU_SETUP", "starting ODU-Low operation controller");
  odu_low->get_operation_controller().start();
  fapi_split_trace::event("RU_SETUP", "ODU-Low operation controller START returned");

  fapi_split_trace::event("RU_SETUP", "starting RU operation controller (OFH/DPDK/NIC bring-up)");
  ru->get_controller().get_operation_controller().start();
  fapi_split_trace::event("RU_SETUP",
                          "RU operation controller START returned — L1<->RU path should be live");

  // Initialize metrics collector.
  if (report_period.count() != 0 && notifier) {
    metrics_collector = split6_o_du_low_metrics_collector_impl(
        odu_low->get_metrics_collector(), ru->get_metrics_collector(), notifier, std::move(timer), report_period);
  }
}
