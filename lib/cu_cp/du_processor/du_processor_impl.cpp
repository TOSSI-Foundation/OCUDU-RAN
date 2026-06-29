// SPDX-FileCopyrightText: Copyright (C) 2021-2026 Software Radio Systems Limited
// SPDX-License-Identifier: BSD-3-Clause-Open-MPI
// Portions of this file may implement 3GPP specifications, which may be subject to additional licensing requirements.

#include "du_processor_impl.h"
#include "ocudu/cu_cp/cu_cp_types.h"
#include "ocudu/f1ap/cu_cp/f1ap_cu_factory.h"
#include "ocudu/ran/cause/f1ap_cause.h"
#include "ocudu/ran/cause/f1ap_cause_converters.h"
#include "ocudu/rrc/rrc_du_factory.h"
#include "ocudu/support/async/coroutine.h"

using namespace ocudu;
using namespace ocucp;

static rrc_cfg_t create_rrc_config(const cu_cp_configuration& cu_cp_cfg)
{
  rrc_cfg_t rrc_cfg;
  rrc_cfg.gnb_id                         = cu_cp_cfg.node.gnb_id;
  rrc_cfg.force_reestablishment_fallback = cu_cp_cfg.rrc.force_reestablishment_fallback;
  rrc_cfg.force_resume_fallback          = cu_cp_cfg.rrc.force_resume_fallback;
  rrc_cfg.rrc_procedure_guard_time_ms    = cu_cp_cfg.rrc.rrc_procedure_guard_time_ms;
  rrc_cfg.int_algo_pref_list             = cu_cp_cfg.security.int_algo_pref_list;
  rrc_cfg.enc_algo_pref_list             = cu_cp_cfg.security.enc_algo_pref_list;
  rrc_cfg.srb2_cfg                       = cu_cp_cfg.bearers.srb2_cfg;
  rrc_cfg.drb_config                     = cu_cp_cfg.bearers.drb_config;
  return rrc_cfg;
}

class du_processor_impl::f1ap_du_processor_adapter : public f1ap_du_processor_notifier
{
public:
  f1ap_du_processor_adapter(du_processor_impl& parent_, common_task_scheduler& common_task_sched_) :
    parent(parent_), common_task_sched(&common_task_sched_)
  {
  }

  du_setup_result on_new_du_setup_request(const du_setup_request& msg) override
  {
    return parent.handle_du_setup_request(msg);
  }

  ue_rrc_context_creation_outcome
  on_ue_rrc_context_creation_request(const ue_rrc_context_creation_request& req) override
  {
    return parent.handle_ue_rrc_context_creation_request(req);
  }

  void on_du_initiated_ue_context_release_request(const f1ap_ue_context_release_request& req) override
  {
    parent.handle_du_initiated_ue_context_release_request(req);
  }

  void on_du_initiated_ue_context_modification_required(
      const f1ap_du_initiated_ue_context_modification_required& req) override
  {
    parent.handle_du_initiated_ue_context_modification_required(req);
  }

  void on_access_success(const f1ap_access_success& msg) override { parent.handle_access_success(msg); }

  bool schedule_async_task(async_task<void> task) override
  {
    return common_task_sched->schedule_async_task(std::move(task));
  }

  async_task<void> on_transaction_info_loss(const ue_transaction_info_loss_event& ev) override
  {
    return parent.cu_cp_notifier.on_transaction_info_loss(ev);
  }

private:
  du_processor_impl&     parent;
  common_task_scheduler* common_task_sched = nullptr;
};

// du_processor_impl

du_processor_impl::du_processor_impl(du_processor_config_t        du_processor_config_,
                                     du_processor_cu_cp_notifier& cu_cp_notifier_,
                                     f1ap_message_notifier&       f1ap_pdu_notifier_,
                                     common_task_scheduler&       common_task_sched_,
                                     ue_manager&                  ue_mng_) :
  cfg(std::move(du_processor_config_)),
  cu_cp_notifier(cu_cp_notifier_),
  f1ap_pdu_notifier(f1ap_pdu_notifier_),
  ue_mng(ue_mng_),
  f1ap_ev_notifier(std::make_unique<f1ap_du_processor_adapter>(*this, common_task_sched_))
{
  // Create F1AP.
  f1ap = create_f1ap(cfg.cu_cp_cfg.f1ap,
                     f1ap_pdu_notifier,
                     *f1ap_ev_notifier,
                     *cfg.cu_cp_cfg.services.timers,
                     *cfg.cu_cp_cfg.services.cu_cp_executor);

  // Create RRC DU.
  rrc = create_rrc_du(create_rrc_config(cfg.cu_cp_cfg));
}

du_setup_result du_processor_impl::handle_du_setup_request(const du_setup_request& request)
{
  du_setup_result res;

  // Extract cell info from served cell list.
  // TODO: How to handle missing optional freq and timing in meas timing config?
  std::map<nr_cell_global_id_t, rrc_cell_info> cell_info_db = rrc->get_cell_info(request.gnb_du_served_cells_list);
  if (cell_info_db.empty()) {
    res.result = du_setup_result::rejected{f1ap_cause_transport_t::unspecified, "Could not extract cell info from DU"};
    return res;
  }

  // Collect PLMN IDs and cell meas config of all served cells.
  std::set<plmn_identity>                              plmn_ids;
  std::map<nr_cell_identity, serving_cell_meas_config> meas_config_db;
  for (const auto& [cgi, cell_info] : cell_info_db) {
    for (const auto& plmn : cell_info.plmn_identity_list) {
      plmn_ids.insert(plmn);
    }

    // Fill cell meas config.
    serving_cell_meas_config meas_cfg;
    meas_cfg.nci               = cgi.nci;
    meas_cfg.gnb_id_bit_length = cfg.cu_cp_cfg.node.gnb_id.bit_length;
    meas_cfg.plmn              = cgi.plmn_id;
    meas_cfg.pci               = cell_info.nr_pci;
    meas_cfg.band              = cell_info.band;
    if (!cell_info.meas_timings.empty() && cell_info.meas_timings.begin()->freq_and_timing.has_value()) {
      // TODO: which meas timing to use when multiple are present?
      const auto& freq_timing = cell_info.meas_timings.begin()->freq_and_timing.value();
      meas_cfg.ssb_mtc        = freq_timing.ssb_meas_timing_cfg;
      meas_cfg.ssb_arfcn      = freq_timing.carrier_freq;
      meas_cfg.ssb_scs        = freq_timing.ssb_subcarrier_spacing;
    }

    meas_config_db.emplace(cgi.nci, meas_cfg);
  }

  // Check if CU-CP can accept a new DU connection.
  if (not cfg.du_setup_notif->on_du_setup_request(cfg.du_index, plmn_ids)) {
    res.result = du_setup_result::rejected{f1ap_cause_radio_network_t::plmn_not_served_by_the_gnb_cu,
                                           "One or more PLMNs are not served by the GNB CU-CP"};
    return res;
  }

  // Validate and update DU configuration.
  auto cfg_res = cfg.du_cfg_hdlr->handle_new_du_config(request);
  if (not cfg_res.has_value()) {
    res.result = cfg_res.error();
    return res;
  }

  // Update cell config in cell measurement manager.
  for (const auto& [nci, meas_config] : meas_config_db) {
    if (not cu_cp_notifier.on_cell_config_update_request(nci, meas_config)) {
      res.result =
          du_setup_result::rejected{f1ap_cause_transport_t::unspecified, "Could not update cell measurement config"};
      return res;
    }
  }

  // Store cell info in RRC DU.
  rrc->store_cell_info_db(cell_info_db);

  // Prepare DU response with accepted setup.
  auto& accepted              = res.result.emplace<du_setup_result::accepted>();
  accepted.gnb_cu_name        = cfg.cu_cp_cfg.node.ran_node_name;
  accepted.gnb_cu_rrc_version = cfg.cu_cp_cfg.rrc.rrc_version;

  // Accept all cells.
  accepted.cells_to_be_activ_list.resize(request.gnb_du_served_cells_list.size());
  for (unsigned i = 0; i != accepted.cells_to_be_activ_list.size(); ++i) {
    accepted.cells_to_be_activ_list[i].nr_cgi = request.gnb_du_served_cells_list[i].served_cell_info.nr_cgi;
    accepted.cells_to_be_activ_list[i].nr_pci = request.gnb_du_served_cells_list[i].served_cell_info.nr_pci;
  }

  return res;
}

bool du_processor_impl::create_rrc_ue(cu_cp_ue&                              ue,
                                      rnti_t                                 c_rnti,
                                      const nr_cell_global_id_t&             cgi,
                                      byte_buffer                            du_to_cu_rrc_container,
                                      std::optional<rrc_ue_transfer_context> rrc_context)
{
  // Create RRC UE to F1AP adapter.
  rrc_ue_f1ap_adapters.emplace(std::piecewise_construct,
                               std::forward_as_tuple(ue.get_ue_index()),
                               std::forward_as_tuple(f1ap->get_f1ap_rrc_message_handler(), ue.get_ue_index()));

  const du_cell_configuration& cell = *cfg.du_cfg_hdlr->get_context().find_cell(cgi);

  // Create new RRC UE entity.
  rrc_ue_creation_message rrc_ue_create_msg{};
  rrc_ue_create_msg.ue_index              = ue.get_ue_index();
  rrc_ue_create_msg.c_rnti                = c_rnti;
  rrc_ue_create_msg.cell.cgi              = cgi;
  rrc_ue_create_msg.cell.tac              = cell.tac;
  rrc_ue_create_msg.cell.pci              = cell.pci;
  rrc_ue_create_msg.cell.bands            = cell.bands;
  rrc_ue_create_msg.f1ap_pdu_notifier     = &rrc_ue_f1ap_adapters.at(ue.get_ue_index());
  rrc_ue_create_msg.ngap_notifier         = &ue.get_rrc_ue_ngap_adapter();
  rrc_ue_create_msg.rrc_ue_cu_cp_notifier = &ue.get_rrc_ue_context_update_notifier();
  rrc_ue_create_msg.measurement_notifier  = &ue.get_rrc_ue_measurement_notifier();
  rrc_ue_create_msg.cu_cp_ue_notifier     = &ue.get_rrc_ue_cu_cp_ue_notifier();
  rrc_ue_create_msg.du_to_cu_container    = std::move(du_to_cu_rrc_container);
  rrc_ue_create_msg.rrc_context           = std::move(rrc_context);
  auto* rrc_ue                            = rrc->add_ue(rrc_ue_create_msg);
  if (rrc_ue == nullptr) {
    logger.warning("Could not create RRC UE");
    return false;
  }

  // Notify CU-CP about the creation of the RRC UE.
  cu_cp_notifier.on_rrc_ue_created(ue.get_ue_index(), *rrc_ue);

  return true;
}

ue_rrc_context_creation_outcome
du_processor_impl::handle_ue_rrc_context_creation_request(const ue_rrc_context_creation_request& req)
{
  ocudu_assert(req.c_rnti != rnti_t::INVALID_RNTI, "ue={} c-rnti={}: Invalid C-RNTI", req.ue_index, req.c_rnti);

  // Check that creation message is valid.
  const du_cell_configuration* pcell = cfg.du_cfg_hdlr->get_context().find_cell(req.cgi);
  if (pcell == nullptr) {
    logger.warning("ue={} c-rnti={}: Could not find cell with nci={}", req.ue_index, req.c_rnti, req.cgi.nci);
    // Return the RRCReject container.
    return make_unexpected(rrc->get_rrc_reject());
  }
  const pci_t pci = pcell->pci;

  ue_index_t ue_index = req.ue_index;
  cu_cp_ue*  ue       = nullptr;

  bool is_resume_request = false;

  if (ue_index == ue_index_t::invalid) {
    // Check if this is a RRC Resume request for an existing UE.
    std::optional<rrc_resume_context_t> resume_context =
        rrc->get_rrc_resume_context(req.rrc_container.copy(), cfg.cu_cp_cfg.ue.nof_i_rnti_ue_bits);
    if (!resume_context.has_value()) {
      logger.warning("ue={}: Could not extract RRC Resume context from UL CCCH Message", req.ue_index);
      // Return the RRCReject container.
      return make_unexpected(rrc->get_rrc_reject());
    }

    if (resume_context->is_resume && resume_context->rrc_resume_id.has_value()) {
      if (std::holds_alternative<short_i_rnti_t>(resume_context->rrc_resume_id.value())) {
        ue_index = ue_mng.get_ue_index(std::get<short_i_rnti_t>(resume_context->rrc_resume_id.value()));
        logger.debug("ue={}: RRC Resume Request with {}",
                     ue_index,
                     std::get<short_i_rnti_t>(resume_context->rrc_resume_id.value()));
      } else {
        ue_index = ue_mng.get_ue_index(std::get<full_i_rnti_t>(resume_context->rrc_resume_id.value()));
        logger.debug("ue={}: RRC Resume Request with {}",
                     ue_index,
                     std::get<full_i_rnti_t>(resume_context->rrc_resume_id.value()));
      }

      if (ue_index != ue_index_t::invalid) {
        if (cfg.cu_cp_cfg.rrc.force_resume_fallback) {
          // RRC Resume fallback forced - do not resume. The DU doesn't have a F1AP UE context, so we also remove it
          // here.
          logger.info("ue={}: RRC Resume fallback forced. Removing F1AP UE context", ue_index);
          f1ap->get_f1ap_ue_context_removal_handler().remove_ue_context(ue_index);
          ue_index = ue_index_t::invalid;
        } else {
          ue_mng.set_active(ue_index);
          is_resume_request = true;
        }
      }
    }

    if (ue_index == ue_index_t::invalid) {
      // RRC Resume not requested or failed - create a new UE.

      // Add new CU-CP UE.
      ue_index = ue_mng.add_ue(cfg.du_index, cfg.du_cfg_hdlr->get_context().id, pci, req.c_rnti, pcell->cell_index);
      if (ue_index == ue_index_t::invalid) {
        logger.warning("CU-CP UE creation failed");
        return make_unexpected(rrc->get_rrc_reject());
      }
    }

    ue = ue_mng.find_ue(ue_index);

    /// NOTE: From this point on the UE exists in the UE manager and must be removed if any error occurs.

  } else {
    ue = ue_mng.set_ue_du_context(ue_index, cfg.du_cfg_hdlr->get_context().id, pci, req.c_rnti, pcell->cell_index);
    if (ue == nullptr) {
      logger.warning("ue={}: Could not create UE context", ue_index);
      // A UE with the same PCI and RNTI already exists, so we don't remove it and only reject the new UE.
      return make_unexpected(rrc->get_rrc_reject());
    }
  }

  // If this is not a RRCResume, create an RRC UE. If the DU-to-CU-RRC-Container is empty, the UE will be rejected.
  if (not is_resume_request) {
    if (not create_rrc_ue(*ue, req.c_rnti, req.cgi, req.du_to_cu_rrc_container.copy(), req.prev_context)) {
      logger.warning("ue={}: Could not create RRC UE object", ue_index);
      // Remove the UE from the UE manager.
      ue_mng.remove_ue(ue_index);
      // Return the RRCReject container.
      return make_unexpected(rrc->get_rrc_reject());
    }

    rrc_ue_interface* rrc_ue         = rrc->find_ue(ue_index);
    f1ap_rrc_dcch_adapters[ue_index] = {};
    f1ap_rrc_ccch_adapters[ue_index] = {};
    f1ap_rrc_ccch_adapters.at(ue_index).connect_rrc_ue(rrc_ue->get_ul_pdu_handler());
    f1ap_rrc_dcch_adapters.at(ue_index).connect_rrc_ue(rrc_ue->get_ul_pdu_handler());
  }

  // Signal back that the UE was successfully created.
  logger.info(
      "ue={} c-rnti={}: UE created{}", ue->get_ue_index(), req.c_rnti, is_resume_request ? " (RRC Resume)" : "");

  return ue_rrc_context_creation_response{ue_index,
                                          &f1ap_rrc_ccch_adapters.at(ue_index),
                                          &f1ap_rrc_dcch_adapters.at(ue_index).get_srb1_notifier(),
                                          &f1ap_rrc_dcch_adapters.at(ue_index).get_srb2_notifier()};
}

void du_processor_impl::handle_du_initiated_ue_context_release_request(const f1ap_ue_context_release_request& request)
{
  ocudu_assert(request.ue_index != ue_index_t::invalid, "Invalid UE index", request.ue_index);

  cu_cp_ue* ue = ue_mng.find_du_ue(request.ue_index);
  if (ue == nullptr) {
    logger.warning("ue={}: Dropping DU initiated UE context release request. UE does not exist", request.ue_index);
    return;
  }

  logger.debug("ue={}: Handling DU initiated UE context release request", request.ue_index);

  // The DU requested a UE release, so we cancel all ongoing RRC transactions for the UE.
  auto* rrc_ue = ue->get_rrc_ue();
  if (rrc_ue == nullptr) {
    logger.warning("ue={}: Dropping DU initiated UE context release request. RRC UE does not exist", request.ue_index);
    return;
  }
  rrc_ue->cancel_all_transactions();

  // Schedule on UE task scheduler.
  ue->get_task_sched().schedule_async_task(
      launch_async([this, request, ue](coro_context<async_task<void>>& ctx) mutable {
        CORO_BEGIN(ctx);

        CORO_AWAIT(cu_cp_notifier.on_ue_release_required(
            {request.ue_index, ue->get_up_resource_manager().get_pdu_sessions(), f1ap_to_ngap_cause(request.cause)}));
        CORO_RETURN();
      }));
}

void du_processor_impl::handle_du_initiated_ue_context_modification_required(
    const f1ap_du_initiated_ue_context_modification_required& request)
{
  ocudu_assert(request.ue_index != ue_index_t::invalid, "Invalid UE index", request.ue_index);

  cu_cp_ue* ue = ue_mng.find_du_ue(request.ue_index);
  if (ue == nullptr) {
    logger.warning("ue={}: Dropping DU initiated UE context modification required. UE does not exist",
                   request.ue_index);
    return;
  }

  auto* rrc_ue = ue->get_rrc_ue();
  if (rrc_ue == nullptr) {
    logger.warning("ue={}: Dropping DU initiated UE context modification required. RRC UE does not exist",
                   request.ue_index);
    return;
  }

  logger.info("ue={}: Handling DU-initiated UE Context Modification Required (adaptive BSR periodicity); applying "
              "CellGroupConfig via RRC Reconfiguration (TS 38.473 §8.3.5.2).",
              request.ue_index);

  // TS 38.331
  rrc_reconfiguration_procedure_request reconf_args;
  rrc_recfg_v1530_ies                   v1530;
  v1530.master_cell_group = request.master_cell_group.copy();
  reconf_args.non_crit_ext.emplace(std::move(v1530));

  ue->get_task_sched().schedule_async_task(
      launch_async([rrc_ue, reconf_args = std::move(reconf_args), ue_index = request.ue_index, this, result = false](
                       coro_context<async_task<void>>& ctx) mutable {
        CORO_BEGIN(ctx);
        CORO_AWAIT_VALUE(result, rrc_ue->handle_rrc_reconfiguration_request(reconf_args));
        if (result) {
          logger.debug("ue={}: BSR periodicity RRC Reconfiguration completed successfully.", ue_index);
        } else {
          logger.info("ue={}: BSR periodicity RRC Reconfiguration failed / not acknowledged by the UE.", ue_index);
        }
        CORO_RETURN();
      }));
}

void du_processor_impl::handle_access_success(const f1ap_access_success& msg)
{
  logger.debug("ue={}: Received Access Success notification from DU for cell plmn={} nci={}",
               msg.ue_index,
               msg.cgi.plmn_id,
               msg.cgi.nci);

  cu_cp_ue* ue = ue_mng.find_du_ue(msg.ue_index);
  if (ue == nullptr) {
    logger.warning("ue={}: Dropping Access Success notification. UE does not exist", msg.ue_index);
    return;
  }

  cu_cp_access_success_indication ind;
  ind.ue_index = msg.ue_index;
  ind.cgi      = msg.cgi;

  ue->get_task_sched().schedule_async_task(launch_async([this, ind](coro_context<async_task<void>>& ctx) mutable {
    CORO_BEGIN(ctx);
    CORO_AWAIT(cu_cp_notifier.on_access_success(ind));
    CORO_RETURN();
  }));
}

bool du_processor_impl::has_cell(pci_t pci)
{
  return cfg.du_cfg_hdlr->get_context().find_cell(pci) != nullptr;
}

bool du_processor_impl::has_cell(nr_cell_global_id_t cgi)
{
  return cfg.du_cfg_hdlr->get_context().find_cell(cgi) != nullptr;
}

async_task<f1ap_gnb_cu_configuration_update_response>
du_processor_impl::handle_configuration_update(const f1ap_gnb_cu_configuration_update& request)
{
  // Update the DU configuration.
  cfg.du_cfg_hdlr->handle_gnb_cu_configuration_update(request);

  return f1ap->handle_gnb_cu_configuration_update(request);
}

std::optional<nr_cell_global_id_t> du_processor_impl::get_cgi(pci_t pci)
{
  const du_cell_configuration* cell = cfg.du_cfg_hdlr->get_context().find_cell(pci);
  if (cell != nullptr) {
    return cell->cgi;
  }
  return std::nullopt;
}

byte_buffer du_processor_impl::get_packed_sib1(nr_cell_global_id_t cgi)
{
  const auto& cells = cfg.du_cfg_hdlr->get_context().served_cells;
  for (const auto& cell : cells) {
    if (cell.cgi == cgi) {
      return cell.sys_info.packed_sib1.copy();
    }
  }
  return byte_buffer{};
}

cu_cp_metrics_report::du_info du_processor_impl::handle_du_metrics_report_request() const
{
  cu_cp_metrics_report::du_info report;
  report.id = gnb_du_id_t::invalid;
  if (cfg.du_cfg_hdlr->has_context()) {
    report.id         = cfg.du_cfg_hdlr->get_context().id;
    const auto& cells = cfg.du_cfg_hdlr->get_context().served_cells;
    for (const auto& cell : cells) {
      report.cells.emplace_back();
      report.cells.back().cgi = cell.cgi;
      report.cells.back().pci = cell.pci;
    }
  }
  // Get RRC metrics.
  rrc->get_rrc_du_metrics_collector().collect_metrics(report.rrc_metrics);

  return report;
}
