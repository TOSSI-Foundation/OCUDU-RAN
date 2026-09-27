// SPDX-FileCopyrightText: Copyright (C) 2021-2026 Software Radio Systems Limited
// SPDX-License-Identifier: BSD-3-Clause-Open-MPI
// Portions of this file may implement 3GPP specifications, which may be subject to additional licensing requirements.

#include "du_positioning_manager_impl.h"
#include "du_positioning_handler_factory.h"
#include "procedures/du_positioning_measurement_procedure.h"
#include "procedures/du_ue_positioning_info_procedure.h"
#include "ocudu/adt/format.h"

using namespace ocudu;
using namespace odu;

du_positioning_manager_impl::du_positioning_manager_impl(const du_manager_params& du_params_,
                                                         du_cell_manager&         cell_mng_,
                                                         du_ue_manager&           ue_mng_,
                                                         ocudulog::basic_logger&  logger_) :
  du_params(du_params_), cell_mng(cell_mng_), ue_mng(ue_mng_), logger(logger_)
{
  (void)logger;
}

du_trp_info_response du_positioning_manager_impl::request_trp_info()
{
  // Update TRP information based on the current cell configuration.
  update_trp_info();

  du_trp_info_response resp;
  resp.trps.reserve(trps.size());
  for (auto& trp : trps) {
    resp.trps.push_back(trp.second);
  }
  return resp;
}

async_task<du_positioning_info_response>
du_positioning_manager_impl::request_positioning_info(const du_positioning_info_request& req)
{
  return launch_async<du_ue_positioning_info_procedure>(req, cell_mng, ue_mng);
}

async_task<du_positioning_meas_response>
du_positioning_manager_impl::request_positioning_measurement(const du_positioning_meas_request& req)
{
  // Update TRP information based on the current cell configuration.
  update_trp_info();

  return launch_async<positioning_measurement_procedure>(req, cell_mng, ue_mng, du_params, trps);
}

/// The cell's DL-PRS as the F1AP PRS Configuration (TS 38.473 Section 9.3.1.177): one resource set with one resource,
/// the same values the scheduler transmits (TS 38.211 Section 7.4.1.7).
static prs_cfg_t make_prs_cfg(const du_cell_config& cell_cfg)
{
  const prs_cell_config&   prs = *cell_cfg.ran.prs;
  const bwp_configuration& bwp = cell_cfg.ran.dl_cfg_common.init_dl_bwp.generic_params;

  prs_res_item_t res;
  res.prs_res_id        = 0;
  res.seq_id            = prs.n_id;
  res.re_offset         = prs.comb_offset;
  res.res_slot_offset   = prs.resource_slot_offset;
  res.res_symbol_offset = prs.start_symbol;
  // Sent omnidirectionally from the cell that also sends the SSB (TS 38.214 5.1.6.5 allows QCL with an SSB).
  res.qcl_info = ssb_t{.pci_nr = cell_cfg.ran.pci, .ssb_idx = std::nullopt};

  prs_resource_set_item_t set;
  set.prs_res_set_id = 0;
  set.scs            = bwp.scs;
  // pRSbandwidth: value v stands for 20 + 4 v PRBs, i.e. 1 -> 24 ... 63 -> 272.
  set.prs_bw              = (prs.crbs.length() - 20) / 4;
  set.start_prb           = prs.crbs.start();
  set.point_a             = cell_cfg.ran.dl_cfg_common.freq_info_dl.absolute_freq_point_a.value();
  set.comb_size           = static_cast<uint8_t>(prs.comb_size);
  set.cp_type             = bwp.cp;
  set.res_set_periodicity = prs.period_slots;
  set.res_set_slot_offset = prs.set_slot_offset;
  // No repetition: factor 1, and the (mandatory in F1AP) time gap at its minimum.
  set.res_repeat_factor = 1;
  set.res_time_gap      = 1;
  set.res_numof_symbols = static_cast<uint8_t>(prs.nof_symbols);
  set.prs_res_tx_pwr    = prs.tx_power_dbm;
  set.prs_res_list.push_back(res);

  prs_cfg_t cfg;
  cfg.prs_res_set_list.push_back(set);
  return cfg;
}

void du_positioning_manager_impl::update_trp_info()
{
  for (unsigned i = 0, e = cell_mng.nof_cells(); i != e; ++i) {
    const du_cell_config& cell_cfg = cell_mng.get_cell_cfg(to_du_cell_index(i));

    du_trp_info trp;
    trp.trp_id = uint_to_trp_id(i + 1); // TRP IDs start from 1.
    trp.pci    = cell_cfg.ran.pci;
    trp.cgi    = cell_cfg.nr_cgi;
    trp.arfcn  = cell_cfg.ran.ul_cfg_common.freq_info_ul.absolute_freq_point_a;
    if (cell_cfg.trp_geo_coordinates.has_value()) {
      geographical_coordinates_t geo_coords;
      geo_coords.trp_position_definition_type = trp_position_direct_t{cell_cfg.trp_geo_coordinates.value()};
      trp.geo_coords                          = geo_coords;
    }
    if (cell_cfg.ran.prs.has_value()) {
      trp.prs_cfg = make_prs_cfg(cell_cfg);
    }
    // insert_or_assign, not insert: this runs before every measurement, and a TRP already in the map would otherwise
    // keep its first snapshot for good.
    trps.insert_or_assign(trp.trp_id, trp);
  }
}
