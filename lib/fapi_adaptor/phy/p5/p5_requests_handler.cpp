// SPDX-FileCopyrightText: Copyright (C) 2021-2026 Software Radio Systems Limited
// SPDX-License-Identifier: BSD-3-Clause-Open-MPI
// Portions of this file may implement 3GPP specifications, which may be subject to additional licensing requirements.

#include "p5_requests_handler.h"
#include "ocudu/fapi/common/error_indication_notifier.h"
#include "ocudu/fapi/p5/p5_messages.h"
#include "ocudu/fapi/p5/p5_responses_notifier.h"
#include "ocudu/fapi_adaptor/phy/p5/phy_fapi_p5_sector_fastpath_adaptor_config.h"
#include "ocudu/phy/upper/upper_phy_operation_controller.h"
#include "ocudu/support/executors/task_executor.h"

using namespace ocudu;
using namespace fapi_adaptor;

namespace {

class p5_notifier_dummy : public fapi::p5_responses_notifier
{
public:
  void on_config_response(const fapi::config_response& msg) override {}
  void on_param_response(const fapi::param_response& msg) override {}
  void on_stop_indication(const fapi::stop_indication& msg) override {}
};

class error_indication_notifier_dummy : public fapi::error_indication_notifier
{
public:
  void on_error_indication(const fapi::error_indication& msg) override {}
};

} // namespace

static p5_notifier_dummy               p5_dummy_notifier;
static error_indication_notifier_dummy dummy_error_notifier;

p5_requests_handler::p5_requests_handler(const phy_fapi_p5_sector_fastpath_adaptor_config&       config,
                                         const phy_fapi_p5_sector_fastpath_adaptor_dependencies& dependencies) :
  sector(config.sector_id),
  logger(dependencies.logger),
  executor(dependencies.executor),
  upper_phy_controller(dependencies.upper_phy_controller),
  p5_notifier(&p5_dummy_notifier),
  error_notifier(&dummy_error_notifier)
{
}

p5_requests_handler::~p5_requests_handler()
{
  stop_manager.stop();
}

void p5_requests_handler::send_param_request(const fapi::param_request& msg)
{
  fapi::param_response response;
  response.error_code = fapi::error_code_id::msg_ok;

  p5_notifier->on_param_response(response);
}

void p5_requests_handler::send_config_request(const fapi::config_request& msg)
{
  logger.warning(
      "[CONFIG.request RX L1] sector={} pci={} scs_common={}kHz cp={} duplex={} "
      "dl_arfcn={} dl_bw={}MHz ul_arfcn={} ul_bw={}MHz num_tx_ant={} num_rx_ant={} "
      "ssb_block_power={}dBm ssb_offset_to_pointA={} ssb_k={} "
      "prach_cfg_idx={} prach_root_seq={} prach_zero_corr_zone={} prach_freq_start={} "
      "tdd_cfg_present={}",
      sector,
      msg.cell_cfg.pci,
      scs_to_khz(msg.cell_cfg.scs_common),
      msg.cell_cfg.cp.to_string(),
      static_cast<unsigned>(msg.cell_cfg.duplex),
      msg.cell_cfg.carrier_cfg.dl_f_ref_arfcn,
      msg.cell_cfg.carrier_cfg.dl_bandwidth,
      msg.cell_cfg.carrier_cfg.ul_f_ref_arfcn,
      msg.cell_cfg.carrier_cfg.ul_bandwidth,
      msg.cell_cfg.carrier_cfg.num_tx_ant,
      msg.cell_cfg.carrier_cfg.num_rx_ant,
      msg.cell_cfg.ssb_cfg.ssb_block_power,
      msg.cell_cfg.ssb_cfg.offset_to_point_A.value(),
      msg.cell_cfg.ssb_cfg.k_ssb.value(),
      msg.cell_cfg.prach_cfg.rach_cfg_generic.prach_config_index,
      msg.cell_cfg.prach_cfg.prach_root_seq_index,
      msg.cell_cfg.prach_cfg.rach_cfg_generic.zero_correlation_zone_config,
      msg.cell_cfg.prach_cfg.rach_cfg_generic.msg1_frequency_start,
      msg.cell_cfg.tdd_ul_dl_cfg_common.has_value());

  fapi::config_response response;
  response.error_code = fapi::error_code_id::msg_ok;

  p5_notifier->on_config_response(response);
}

void p5_requests_handler::send_start_request(const fapi::start_request& msg)
{
  if (!executor.defer([this, token = stop_manager.get_token()]() { upper_phy_controller.start(); })) {
    logger.warning("Sector #{}: PHY-FAPI P5 sector adaptor failed to enqueue start task ", sector);
  }
}

void p5_requests_handler::send_stop_request(const fapi::stop_request& msg)
{
  if (!executor.defer([this, token = stop_manager.get_token()]() {
        upper_phy_controller.stop();

        fapi::stop_indication indication;
        p5_notifier->on_stop_indication(indication);
      })) {
    logger.warning("Sector #{}: PHY-FAPI P5 sector adaptor failed to enqueue stop task ", sector);
  }
}
