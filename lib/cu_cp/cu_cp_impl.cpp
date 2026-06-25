// SPDX-FileCopyrightText: Copyright (C) 2021-2026 Software Radio Systems Limited
// SPDX-License-Identifier: BSD-3-Clause-Open-MPI
// Portions of this file may implement 3GPP specifications, which may be subject to additional licensing requirements.

#include "cu_cp_impl.h"
#include "du_processor/du_processor_repository.h"
#include "metrics_handler/metrics_handler_impl.h"
#include "routines/amf_connection_loss_routine.h"
#include "routines/cell_activation_routine.h"
#include "routines/initial_context_setup_routine.h"
#include "routines/mobility/cho_cancellation_routine.h"
#include "routines/mobility/cho_coordinator_routine.h"
#include "routines/mobility/cho_source_routine.h"
#include "routines/mobility/cho_target_routine.h"
#include "routines/mobility/inter_cu_handover_execution_target_routine.h"
#include "routines/mobility/inter_cu_handover_source_routine.h"
#include "routines/mobility/inter_cu_handover_target_routine.h"
#include "routines/mobility/intra_cu_handover_routine.h"
#include "routines/mobility/intra_cu_handover_target_routine.h"
#include "routines/pdu_session_resource_modification_routine.h"
#include "routines/pdu_session_resource_release_routine.h"
#include "routines/pdu_session_resource_setup_routine.h"
#include "routines/positioning/trp_information_exchange_routine.h"
#include "routines/reestablishment_context_modification_routine.h"
#include "routines/ue_amf_context_release_request_routine.h"
#include "routines/ue_context_release_routine.h"
#include "routines/ue_removal_routine.h"
#include "routines/ue_resume_routine.h"
#include "routines/ue_suspend_routine.h"
#include "routines/ue_transaction_info_release_routine.h"
#include "ocudu/cu_cp/cu_cp_types.h"
#include "ocudu/f1ap/cu_cp/f1ap_cu.h"
#include "ocudu/ngap/ngap_location_reporting.h"
#include "ocudu/nrppa/nrppa.h"
#include "ocudu/nrppa/nrppa_factory.h"
#include "ocudu/ran/cause/common.h"
#include "ocudu/ran/cause/ngap_cause.h"
#include "ocudu/ran/plmn_identity.h"
#include "ocudu/ran/time/radio_frame.h"
#include "ocudu/support/async/coroutine.h"
#include "ocudu/support/synchronization/sync_event.h"
#include <chrono>
#include <dlfcn.h>
#include <future>
#include <thread>

using namespace ocudu;
using namespace ocucp;

static void assert_cu_cp_configuration_valid(const cu_cp_configuration& cfg)
{
  ocudu_assert(cfg.services.cu_cp_executor != nullptr, "Invalid CU-CP executor");
  ocudu_assert(!cfg.ngap.ngaps.empty(), "No NGAPs configured");
  for (const auto& ngap : cfg.ngap.ngaps) {
    ocudu_assert(ngap.n2_gw != nullptr, "Invalid N2 GW client handler");
  }
  if (!cfg.xnap.xnaps.empty()) {
    ocudu_assert(cfg.xnap.xnc_gw != nullptr, "Invalid XN-C GW client handler");
  }
  ocudu_assert(cfg.services.timers != nullptr, "Invalid timers");

  report_error_if_not(cfg.admission.max_nof_dus <= MAX_NOF_DUS, "Invalid max number of DUs");
  report_error_if_not(cfg.admission.max_nof_cu_ups <= MAX_NOF_CU_UPS, "Invalid max number of CU-UPs");
}

cu_cp_impl::cu_cp_impl(const cu_cp_configuration& config_) :
  cfg(config_),
  ue_mng(cfg),
  cell_meas_mng(cfg.mobility.meas_manager_config, cell_meas_mobility_notifier, ue_mng),
  du_db(du_repository_config{cfg,
                             *this,
                             get_cu_cp_measurement_config_handler(),
                             get_cu_cp_ue_removal_handler(),
                             get_cu_cp_ue_context_handler(),
                             common_task_sched,
                             ue_mng,
                             conn_notifier,
                             ocudulog::fetch_basic_logger("CU-CP")}),
  cu_up_db(cu_up_repository_config{cfg, e1ap_ev_notifier, common_task_sched, ocudulog::fetch_basic_logger("CU-CP")}),
  paging_handler(du_db),
  ngap_db(ngap_repository_config{cfg, get_cu_cp_ngap_handler(), paging_handler, ocudulog::fetch_basic_logger("CU-CP")}),
  xnap_db(xnap_repository_config{cfg, ocudulog::fetch_basic_logger("CU-CP")}),
  mobility_mng(cfg.mobility.mobility_manager_config,
               mobility_manager_ev_notifier,
               ngap_db,
               du_db,
               ue_mng,
               cell_meas_mng),
  controller(cfg,
             get_cu_cp_amf_reconnection_handler(),
             common_task_sched,
             ngap_db,
             cu_up_db,
             du_db,
             xnap_db,
             *cfg.services.cu_cp_executor),
  metrics_hdlr(std::make_unique<metrics_handler_impl>(*cfg.services.cu_cp_executor,
                                                      *cfg.services.timers,
                                                      ue_mng,
                                                      du_db,
                                                      ngap_db,
                                                      mobility_mng)),
  cu_cp_cfgtr(mobility_manager_ev_notifier, du_db, ngap_db, ue_mng)
{
  assert_cu_cp_configuration_valid(cfg);

  nrppa_entity = create_nrppa(cfg, nrppa_cu_cp_ev_notifier, common_task_sched);

  // Connect event notifiers to layers.
  ngap_cu_cp_ev_notifier.connect_cu_cp(get_cu_cp_ngap_handler(), paging_handler);
  nrppa_cu_cp_ev_notifier.connect_cu_cp(get_cu_cp_nrppa_handler());
  mobility_manager_ev_notifier.connect_cu_cp(get_cu_cp_mobility_manager_handler());
  e1ap_ev_notifier.connect_cu_cp(get_cu_cp_e1ap_handler());
  cell_meas_mobility_notifier.connect_mobility_manager(mobility_mng);

  conn_notifier.connect_node_connection_handler(controller);

  // Start statistics report timer.
  statistics_report_timer = cfg.services.timers->create_unique_timer(*cfg.services.cu_cp_executor);
  statistics_report_timer.set(cfg.metrics.statistics_report_period,
                              [this](timer_id_t /*tid*/) { on_statistics_report_timer_expired(); });
  statistics_report_timer.run();
  if (cfg.metrics_notifier != nullptr and cfg.metrics.metrics_report_period.count() != 0) {
    periodic_metric_report_request metric_cfg{cfg.metrics.metrics_report_period, cfg.metrics_notifier};
    metrics_session = metrics_hdlr->create_periodic_report_session(metric_cfg);
  }
}

cu_cp_impl::~cu_cp_impl()
{
  stop();
}

bool cu_cp_impl::start()
{
  std::promise<bool> p;
  std::future<bool>  fut = p.get_future();

  if (not cfg.services.cu_cp_executor->execute([this, &p]() {
        // Start AMF connection procedure.
        controller.amf_connection_handler().connect_to_amf(&p);
      })) {
    report_fatal_error("Failed to initiate CU-CP setup");
  }
  // Block waiting for CU-CP setup to complete.
  if (not fut.get()) {
    return false; // Could not connect to AMF.
  }

  // Setup succeeded, add XNAPs and try to connect to peers.
  if (not cfg.services.cu_cp_executor->execute([this]() {
        uint32_t xnc_idx = 0;
        for (const auto& xnap : cfg.xnap.xnaps) {
          xnap_configuration xnc_cfg{.gnb_id           = cfg.node.gnb_id,
                                     .tai_support_list = ngap_db.get_supported_tracking_areas(),
                                     .guami_list       = ngap_db.get_served_guamis()};

          // TODO: pass init tx notifier.
          xnap_interface* xnap_entity = xnap_db.add_xnap(uint_to_xnc_peer_index(xnc_idx), xnap.peer_addr, xnc_cfg);
          if (xnap_entity == nullptr) {
            report_fatal_error("Failed to create XNAP entity for peer address {}", xnap.peer_addr);
          }
          ++xnc_idx;
        }

        // Initialize CU-CP XNC connection procedures.
        controller.xnc_connection_handler().start();
      })) {
    report_fatal_error("Failed to initiate XNC CU-CP setup");
  }
  return true;
}

void cu_cp_impl::stop()
{
  bool already_stopped = stopped.exchange(true);
  if (already_stopped) {
    return;
  }
  logger.info("Stopping CU-CP...");

  // Shut down components from within CU-CP executor.
  sync_event ev;
  while (not cfg.services.cu_cp_executor->execute([this, token = ev.get_token()]() {
    // Stop statistics gathering.
    statistics_report_timer.stop();
    if (metrics_session != nullptr) {
      metrics_session->stop();
    }
  })) {
    logger.debug("Failed to dispatch CU-CP stop task. Retrying...");
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
  }
  ev.wait();

  controller.stop();
  logger.info("CU-CP stopped successfully");
}

ngap_message_handler* cu_cp_impl::get_ngap_message_handler(const plmn_identity& plmn)
{
  return ngap_db.find_ngap(plmn);
}

bool cu_cp_impl::amfs_are_connected()
{
  if (cfg.ngap.no_core) {
    return true;
  }

  for (const auto& [amf_index, ngap] : ngap_db.get_ngaps()) {
    if (not controller.amf_connection_handler().is_amf_connected(amf_index)) {
      return false;
    }
  }

  return true;
}

void cu_cp_impl::handle_bearer_context_release_request(const cu_cp_bearer_context_release_request& msg)
{
  cu_cp_ue* ue = ue_mng.find_du_ue(msg.ue_index);
  ocudu_assert(ue != nullptr, "ue={}: Could not find DU UE", msg.ue_index);

  if (ue->get_handover_ue_release_timer().is_running()) {
    logger.debug("ue={}: Ignoring Bearer Context Release Request. Cause: Ongoing handover for this UE", msg.ue_index);
    return;
  }

  request_ue_release(*ue, msg.cause);
}

void cu_cp_impl::handle_bearer_context_inactivity_notification(const cu_cp_inactivity_notification& msg)
{
  if (msg.ue_inactive) {
    cu_cp_ue* ue = ue_mng.find_du_ue(msg.ue_index);
    ocudu_assert(ue != nullptr, "ue={}: Could not find DU UE", msg.ue_index);

    if (ue->get_handover_ue_release_timer().is_running()) {
      logger.debug("ue={}: Ignoring Bearer Context Inactivity Notification. Cause: Ongoing handover for this UE",
                   msg.ue_index);
      return;
    }

    auto* ngap = ngap_db.find_ngap(ue->get_ue_context().plmn);
    if (ngap == nullptr) {
      logger.warning("ue={}: Dropping Bearer Context Inactivity Notification. NGAP not found for plmn={}",
                     msg.ue_index,
                     ue->get_ue_context().plmn);
      return;
    }

    // Get Core Network Assist Info for Inactive if present.
    std::optional<ngap_core_network_assist_info_for_inactive> cn_assist_info_for_inactive =
        ngap->get_cn_assist_info_for_inactive(msg.ue_index);

    // To set the UE as inactive, the following conditions must be met:
    // 1. RRC Inactive must be configured in CU-CP.
    // 2. RRC Inactive must be supported by the UE.
    // 3. The Core Network Assist Info for Inactive from the AMF must be present.
    // Otherwise the RAN Paging procedure cannot be performed.
    if (cfg.ue.enable_rrc_inactive && ue->get_rrc_ue()->is_rrc_inactive_supported() &&
        cn_assist_info_for_inactive.has_value()) {
      // Set UE as inactive.
      std::optional<i_rntis_t> i_rntis = ue_mng.set_inactive(ue->get_ue_index());
      if (i_rntis.has_value()) {
        logger.debug("ue={}: Set UE as inactive with {} {}",
                     msg.ue_index,
                     i_rntis.value().short_i_rnti,
                     i_rntis.value().full_i_rnti);

        du_processor& du_proc = du_db.get_du_processor(ue->get_du_index());

        // Add all local cells to RAN area cells.
        // Note: Support for configurable ran area cells is left as future work.
        rrc_inactivity_context inactivity_ctx{.i_rntis                 = i_rntis.value(),
                                              .next_hop_chaining_count = ue->get_security_manager().get_ncc(),
                                              .ran_paging_cycle        = cfg.ue.ran_paging_cycle,
                                              .ran_notification_area_info =
                                                  du_proc.get_rrc_du_handler().get_ran_area_cells(),
                                              .t380 = cfg.ue.t380};

        rrc_ue_release_context release_context =
            ue->get_rrc_ue()->get_rrc_ue_release_context(true, std::nullopt, inactivity_ctx);

        // Schedule on UE task scheduler.
        ue->get_task_sched().schedule_async_task(launch_async<ue_suspend_routine>(
            ue->get_ue_index(),
            std::move(release_context),
            cu_up_db.find_cu_up_processor(ue->get_cu_up_index())->get_e1ap_bearer_context_manager(),
            du_db.get_du_processor(ue->get_du_index()).get_f1ap_handler(),
            get_cu_cp_ngap_handler(),
            du_db.get_du_processor(ue->get_du_index()).get_rrc_du_handler().get_rrc_du_connection_event_handler(),
            ngap->get_ngap_control_message_handler(),
            get_cu_cp_ue_context_handler(),
            logger));
      }
    } else {
      // Inactivity is not possible, so the UE must be released.
      if (!cfg.ue.enable_rrc_inactive || !ue->get_rrc_ue()->is_rrc_inactive_supported()) {
        logger.debug("ue={}: Releasing UE due to inactivity notification. RRC Inactive is not {}",
                     msg.ue_index,
                     cfg.ue.enable_rrc_inactive ? "supported by the UE" : "configured");
      } else {
        logger.debug("ue={}: Releasing UE due to inactivity notification. RRC Inactive is not possible without Core "
                     "Network Assist Info for Inactive",
                     msg.ue_index);
      }

      request_ue_release(*ue, ngap_cause_radio_network_t::user_inactivity);
    }
  } else {
    logger.debug("Inactivity notification level not supported");
  }
}

void cu_cp_impl::handle_dl_data_notification(ue_index_t ue_index)
{
  std::optional<full_i_rnti_t> full_i_rnti = ue_mng.get_full_i_rnti(ue_index);
  if (!full_i_rnti.has_value()) {
    logger.warning("ue={}: Dropping DL Data Notification. I-RNTI for UE not found", ue_index);
    return;
  }

  // Send paging message.
  send_ran_paging(ue_index, full_i_rnti.value());
}

void cu_cp_impl::handle_e1_release_request(cu_up_index_t cu_up_index)
{
  // TODO
}

bool cu_cp_impl::handle_ue_plmn_selected(ue_index_t ue_index, const plmn_identity& plmn)
{
  if (!controller.is_supported_plmn(plmn)) {
    logger.warning("ue={}: PLMN {} not supported, rejecting UE", ue_index, plmn);
    return false;
  }

  if (!ue_mng.set_plmn(ue_index, plmn)) {
    logger.error("ue={}: Could not set PLMN {}", ue_index, plmn);
    return false;
  }

  // Connect NGAP to RRC UE to NGAP adapter.
  logger.debug("ue={}: Connecting NGAP (plmn={}) to RRC UE adapter", ue_index, plmn);
  ue_mng.get_rrc_ue_ngap_adapter(ue_index).connect_ngap(ngap_db.find_ngap(plmn));

  return true;
}

rrc_ue_reestablishment_context_response
cu_cp_impl::handle_rrc_reestablishment_request(pci_t old_pci, rnti_t old_c_rnti, ue_index_t ue_index)
{
  rrc_ue_reestablishment_context_response reest_context{};

  ue_index_t old_ue_index = ue_mng.get_ue_index(old_pci, old_c_rnti);
  if (old_ue_index == ue_index_t::invalid || old_ue_index == ue_index) {
    return reest_context;
  }

  auto* const old_ue = ue_mng.find_du_ue(old_ue_index);
  if (old_ue == nullptr) {
    logger.debug("ue={}: Could not find UE", old_ue_index);
    return reest_context;
  }

  // Stop the UE release timer if it is running.
  if (old_ue->get_handover_ue_release_timer().is_running()) {
    logger.debug("ue={}: Stopping handover UE release timer", old_ue_index);
    old_ue->get_handover_ue_release_timer().stop();
  }

  // Cancel any ongoing handover transaction for the UE.
  if (old_ue->get_ho_context().has_value()) {
    logger.debug("ue={}: Cancelling handover transaction", old_ue_index);

    auto* const target_ue = ue_mng.find_du_ue(old_ue->get_ho_context()->target_ue_index);
    if (target_ue == nullptr) {
      logger.debug("ue={}: Could not find UE", old_ue->get_ho_context()->target_ue_index);
    } else {
      target_ue->get_rrc_ue()->cancel_handover_reconfiguration_transaction(
          old_ue->get_ho_context().value().rrc_reconfig_transaction_id);
    }
  }

  // Check if a DRB and SRB2 were setup.
  if (old_ue->get_up_resource_manager().get_drbs().empty()) {
    logger.debug("ue={}: No DRB setup for this UE - rejecting RRC reestablishment", old_ue_index);
    reest_context.ue_index = old_ue_index;
    return reest_context;
  }

  auto srbs = old_ue->get_rrc_ue()->get_srbs();
  if (std::find(srbs.begin(), srbs.end(), srb_id_t::srb2) == srbs.end()) {
    logger.debug("ue={}: SRB2 not setup for this UE - rejecting RRC reestablishment", old_ue_index);
    reest_context.ue_index = old_ue_index;
    return reest_context;
  }

  auto* rrc_ue = old_ue->get_rrc_ue();
  if (rrc_ue == nullptr) {
    logger.debug("ue={}: RRC UE not found for this UE - rejecting RRC reestablishment", old_ue_index);
    reest_context.ue_index = old_ue_index;
    return reest_context;
  }

  // Get RRC Reestablishment UE Context from old UE.
  reest_context                       = rrc_ue->get_context();
  reest_context.old_ue_fully_attached = true;
  reest_context.ue_index              = old_ue_index;

  return reest_context;
}

async_task<bool> cu_cp_impl::handle_rrc_reestablishment_context_modification_required(ue_index_t ue_index)
{
  cu_cp_ue* ue = ue_mng.find_du_ue(ue_index);
  ocudu_assert(ue != nullptr, "ue={}: Could not find DU UE", ue_index);
  ocudu_assert(ue->get_cu_up_index() != cu_up_index_t::invalid, "ue={}: could not find CU-UP of the UE", ue_index);

  return launch_async<reestablishment_context_modification_routine>(
      ue_index,
      ue->get_security_manager().get_up_as_config(),
      cu_up_db.find_cu_up_processor(ue->get_cu_up_index())->get_e1ap_bearer_context_manager(),
      du_db.get_du_processor(ue->get_du_index()).get_f1ap_handler(),
      ue->get_rrc_ue(),
      get_cu_cp_rrc_ue_interface(),
      ue->get_task_sched(),
      ue->get_up_resource_manager(),
      get_cu_cp_location_manager_handler(),
      logger);
}

void cu_cp_impl::handle_rrc_reestablishment_failure(const cu_cp_ue_context_release_request& request)
{
  auto* ue = ue_mng.find_ue(request.ue_index);
  if (ue != nullptr) {
    ue->get_task_sched().schedule_async_task(handle_ue_context_release(request));
  };
}

void cu_cp_impl::handle_rrc_reestablishment_complete(ue_index_t old_ue_index)
{
  auto* ue = ue_mng.find_ue(old_ue_index);
  if (ue != nullptr) {
    ue->get_task_sched().schedule_async_task(handle_ue_removal_request(old_ue_index));
  };
}

void cu_cp_impl::handle_rrc_reconf_complete_indicator(ue_index_t ue_index)
{
  cu_cp_ue* ue = ue_mng.find_du_ue(ue_index);
  ocudu_assert(ue != nullptr, "ue={}: Could not find DU UE", ue_index);

  if (ue != nullptr) {
    ue->get_task_sched().schedule_async_task(
        launch_async([this, ue_index, ue_context_mod_request = f1ap_ue_context_modification_request{}](
                         coro_context<async_task<void>>& ctx) mutable {
          CORO_BEGIN(ctx);

          if (ue_mng.find_du_ue(ue_index) == nullptr) {
            CORO_EARLY_RETURN();
          }

          ue_context_mod_request.ue_index               = ue_index;
          ue_context_mod_request.rrc_recfg_complete_ind = f1ap_rrc_recfg_complete_ind::true_value;

          CORO_AWAIT(du_db.get_du_processor(ue_mng.find_du_ue(ue_index)->get_du_index())
                         .get_f1ap_handler()
                         .handle_ue_context_modification_request(ue_context_mod_request));

          CORO_RETURN();
        }));
  }
}

async_task<bool> cu_cp_impl::handle_ue_context_transfer(ue_index_t ue_index, ue_index_t old_ue_index)
{
  if (cu_up_db.get_nof_cu_ups() == 0) {
    logger.warning("No CU-UP connected");
    return launch_async([](coro_context<async_task<bool>>& ctx) {
      CORO_BEGIN(ctx);
      CORO_RETURN(false);
    });
  }

  if (ue_mng.find_ue(ue_index) == nullptr) {
    logger.warning("ue={} not found", ue_index);
    return launch_async([](coro_context<async_task<bool>>& ctx) {
      CORO_BEGIN(ctx);
      CORO_RETURN(false);
    });
  }

  cu_cp_ue* old_ue = ue_mng.find_du_ue(old_ue_index);
  if (old_ue == nullptr) {
    logger.warning("Old UE index={} got removed", old_ue_index);
    return launch_async([](coro_context<async_task<bool>>& ctx) {
      CORO_BEGIN(ctx);
      CORO_RETURN(false);
    });
  }

  // Cancel all ongoing RRC transactions of the old UE.
  old_ue->get_rrc_ue()->get_rrc_ue_control_message_handler().cancel_all_transactions();

  // Task to run in old UE task scheduler.
  auto handle_ue_context_transfer_impl = [this, ue_index, old_ue_index]() {
    if (ue_mng.find_du_ue(old_ue_index) == nullptr) {
      logger.warning("Old UE index={} got removed", old_ue_index);
      return false;
    }

    auto* source_ue = ue_mng.find_du_ue(old_ue_index);
    if (source_ue->get_cu_up_index() == cu_up_index_t::invalid) {
      logger.warning("ue={}: could not find CU-UP of the old UE", old_ue_index);
      return false;
    }

    if (ue_mng.find_du_ue(ue_index) == nullptr) {
      logger.warning("UE index={} got removed", ue_index);
      return false;
    }

    auto* ue = ue_mng.find_du_ue(ue_index);

    // Transfer CU-UP index.
    ue->set_cu_up_index(source_ue->get_cu_up_index());

    // Transfer source F1AP UE context to F1AP.
    if (source_ue->get_du_index() == ue->get_du_index()) {
      const bool result = du_db.get_du_processor(source_ue->get_du_index())
                              .get_f1ap_handler()
                              .handle_ue_id_update(ue_index, old_ue_index);
      if (not result) {
        logger.warning("The F1AP UE context of the old UE index {} does not exist", old_ue_index);
        return false;
      }
    }

    auto* ngap = ngap_db.find_ngap(ue->get_ue_context().plmn);
    if (ngap == nullptr) {
      logger.warning(
          "ue={}: Can't transfer UE context. Cause: NGAP not found for plmn={}", ue_index, ue->get_ue_context().plmn);
      return false;
    }

    // Transfer NGAP UE Context to new UE and remove the old context.
    if (not ngap->update_ue_index(ue_index, old_ue_index, ue_mng.find_ue(ue_index)->get_ngap_cu_cp_ue_notifier())) {
      return false;
    }

    // Connect NGAP to RRC UE to NGAP adapter.
    logger.debug("ue={}: Connecting NGAP (plmn={}) to RRC UE adapter", ue_index, ue->get_ue_context().plmn);
    ue_mng.get_rrc_ue_ngap_adapter(ue_index).connect_ngap(ngap);

    // Transfer E1AP UE Context to new UE and remove old context.
    cu_up_db.find_cu_up_processor(source_ue->get_cu_up_index())->update_ue_index(ue_index, old_ue_index);

    // Transfer location reporting configuration from source UE to new UE.
    ue->get_location_manager().set_config(source_ue->get_location_manager().get_config());

    return true;
  };

  // Task that the caller will use to sync with the old UE task scheduler.
  struct transfer_context_task {
    transfer_context_task(cu_cp_impl& parent_, ue_index_t old_ue_index_, unique_function<bool()> callable) :
      parent(parent_),
      old_ue_index(old_ue_index_),
      task([this, callable = std::move(callable)]() { transfer_successful = callable(); })
    {
    }

    void operator()(coro_context<async_task<bool>>& ctx)
    {
      CORO_BEGIN(ctx);

      CORO_AWAIT_VALUE(
          const bool task_run,
          parent.ue_mng.get_task_sched().dispatch_and_await_task_completion(old_ue_index, std::move(task)));

      CORO_RETURN(task_run and transfer_successful);
    }

    cu_cp_impl& parent;
    ue_index_t  old_ue_index;
    unique_task task;

    bool transfer_successful = false;
  };

  return launch_async<transfer_context_task>(*this, old_ue_index, handle_ue_context_transfer_impl);
}

void cu_cp_impl::handle_handover_reconfiguration_sent(const cu_cp_intra_cu_handover_target_request& request)
{
  if (ue_mng.find_du_ue(request.target_ue_index) == nullptr) {
    logger.warning("UE index={} got removed", request.target_ue_index);
    return;
  }

  cu_cp_ue* ue = ue_mng.find_du_ue(request.target_ue_index);

  ue->get_task_sched().schedule_async_task(launch_async<intra_cu_handover_target_routine>(
      request,
      cu_up_db.find_cu_up_processor(ue->get_cu_up_index())->get_e1ap_bearer_context_manager(),
      du_db.get_du_processor(ue->get_du_index()).get_f1ap_handler(),
      *this,
      get_cu_cp_ue_removal_handler(),
      *this,
      get_cu_cp_location_manager_handler(),
      ue_mng,
      mobility_mng,
      logger));
}

void cu_cp_impl::handle_cho_reconfiguration_sent(const cu_cp_cho_target_request& request)
{
  if (ue_mng.find_du_ue(request.target_ue_index) == nullptr) {
    logger.warning("CHO target UE index={} got removed", request.target_ue_index);
    return;
  }

  cu_cp_ue* ue = ue_mng.find_du_ue(request.target_ue_index);

  // Schedule cho_target_routine on the target UE's task scheduler.
  // This routine only waits for target-side RRCReconfigurationComplete.
  // Source-side CHO completion is handled separately (Access Success / cho_source_routine).
  ue->get_task_sched().schedule_async_task(launch_async<cho_target_routine>(request, ue_mng, logger));
}

void cu_cp_impl::handle_handover_ue_context_push(ue_index_t source_ue_index, ue_index_t target_ue_index)
{
  auto* ue = ue_mng.find_ue(target_ue_index);
  ocudu_assert(ue != nullptr, "ue={} not found", target_ue_index);
  ocudu_assert(
      ue->get_cu_up_index() != cu_up_index_t::invalid, "ue={}: could not find CU-UP of the target UE", target_ue_index);

  auto* ngap = ngap_db.find_ngap(ue->get_ue_context().plmn);
  if (ngap == nullptr) {
    logger.warning(
        "ue={}: could not find NGAP of the target UE for plmn={}", target_ue_index, ue->get_ue_context().plmn);
    return;
  }

  // Transfer NGAP UE Context to new UE and remove the old context.
  if (!ngap->update_ue_index(target_ue_index, source_ue_index, ue->get_ngap_cu_cp_ue_notifier())) {
    return;
  }
  // Transfer E1AP UE Context to new UE and remove old context.
  cu_up_db.find_cu_up_processor(ue->get_cu_up_index())->update_ue_index(target_ue_index, source_ue_index);

  // Transfer location reporting configuration from source UE to target UE.
  auto* source_ue = ue_mng.find_ue(source_ue_index);
  if (source_ue != nullptr) {
    ue->get_location_manager().set_config(source_ue->get_location_manager().get_config());
  }
}

async_task<void> cu_cp_impl::handle_ue_context_release(const cu_cp_ue_context_release_request& request)
{
  auto* ue = ue_mng.find_ue(request.ue_index);
  if (ue == nullptr) {
    logger.warning("ue={}: Could not find UE", request.ue_index);
    return launch_async([](coro_context<async_task<void>>& ctx) {
      CORO_BEGIN(ctx);
      CORO_RETURN();
    });
  }

  auto* ngap = ngap_db.find_ngap(ue->get_ue_context().plmn);

  return launch_async<ue_amf_context_release_request_routine>(
      request, ngap ? &ngap->get_ngap_control_message_handler() : nullptr, *this, logger);
}

async_task<void> cu_cp_impl::handle_access_success(const cu_cp_access_success_indication& msg)
{
  return launch_async<cho_source_routine>(msg, ue_mng, du_db, cu_up_db, *this, *this, mobility_mng, logger);
}

async_task<rrc_resume_request_response> cu_cp_impl::handle_rrc_resume_request(const cu_cp_rrc_resume_request& request)
{
  cu_cp_ue* ue = ue_mng.find_du_ue(request.ue_index);
  ocudu_assert(ue != nullptr, "ue={}: Could not find DU UE", request.ue_index);

  // Stop RAN paging timer if it is running.
  unique_timer& ran_paging_timer = ue->get_ran_paging_timer();
  if (ran_paging_timer.is_running()) {
    logger.debug("ue={}: Stopping RAN paging timer", request.ue_index);
    ran_paging_timer.stop();
  }

  // Stop RNA update timer if it is running.
  unique_timer& rna_update_timer = ue->get_rna_update_timer();
  if (rna_update_timer.is_running()) {
    logger.debug("ue={}: Stopping RNA update timer", request.ue_index);
    rna_update_timer.stop();
  }

  return launch_async<ue_resume_routine>(request,
                                         cfg.ue,
                                         du_db.get_du_processor(ue->get_du_index()),
                                         get_cu_cp_ue_context_handler(),
                                         cu_up_db.find_cu_up_processor(ue->get_cu_up_index())->get_e1ap_handler(),
                                         ue_mng,
                                         get_cu_cp_location_manager_handler(),
                                         logger);
}

void cu_cp_impl::handle_ran_paging_required(ue_index_t ue_index)
{
  std::optional<full_i_rnti_t> full_i_rnti = ue_mng.get_full_i_rnti(ue_index);
  if (!full_i_rnti.has_value()) {
    logger.warning("ue={}: Dropping DL NAS transport message. I-RNTI for UE not found", ue_index);
    return;
  }

  // Send paging message.
  send_ran_paging(ue_index, full_i_rnti.value());
}

bool cu_cp_impl::handle_handover_request(ue_index_t                        ue_index,
                                         const plmn_identity&              selected_plmn,
                                         const security::security_context& sec_ctxt)
{
  cu_cp_ue* ue = ue_mng.find_ue(ue_index);
  if (ue == nullptr) {
    logger.debug("ue={}: Could not find UE", ue_index);
    return false;
  }

  if (!handle_ue_plmn_selected(ue_index, selected_plmn)) {
    logger.info("ue={}: PLMN selection failed", ue_index);
    return false;
  }

  if (!ue->get_security_manager().init_security_context(sec_ctxt)) {
    logger.info("ue={}: Security context initialization failed", ue_index);
    return false;
  }

  return true;
}

async_task<expected<ngap_init_context_setup_response, ngap_init_context_setup_failure>>
cu_cp_impl::handle_new_initial_context_setup_request(const ngap_init_context_setup_request& request)
{
  cu_cp_ue* ue = ue_mng.find_du_ue(request.ue_index);
  ocudu_assert(ue != nullptr, "ue={}: Could not find UE", request.ue_index);
  rrc_ue_interface* rrc_ue = ue->get_rrc_ue();
  ocudu_assert(rrc_ue != nullptr, "ue={}: Could not find RRC UE", request.ue_index);

  auto* ngap = ngap_db.find_ngap(ue->get_ue_context().plmn);
  if (ngap == nullptr) {
    logger.warning("ue={}: Initial context setup failed. Cause: NGAP not found for plmn={}",
                   request.ue_index,
                   ue->get_ue_context().plmn);
    return launch_async(
        [](coro_context<async_task<expected<ngap_init_context_setup_response, ngap_init_context_setup_failure>>>& ctx) {
          CORO_BEGIN(ctx);
          CORO_RETURN(make_unexpected(ngap_init_context_setup_failure{}));
        });
  }

  return launch_async<initial_context_setup_routine>(request,
                                                     *rrc_ue,
                                                     ngap->get_ngap_ue_radio_cap_management_handler(),
                                                     ue->get_security_manager(),
                                                     ue->get_location_manager(),
                                                     du_db.get_du_processor(ue->get_du_index()).get_f1ap_handler(),
                                                     get_cu_cp_ngap_handler(),
                                                     logger);
}

async_task<cu_cp_pdu_session_resource_setup_response>
cu_cp_impl::handle_new_pdu_session_resource_setup_request(cu_cp_pdu_session_resource_setup_request& request)
{
  cu_cp_ue* ue = ue_mng.find_du_ue(request.ue_index);
  ocudu_assert(ue != nullptr, "ue={}: Could not find DU UE", request.ue_index);

  // Select a CU-UP to serve the UE if it is not already assigned.
  if (ue->get_cu_up_index() == cu_up_index_t::invalid) {
    ue->set_cu_up_index(cu_up_db.select_cu_up());
  }
  ocudu_assert(ue->get_cu_up_index() != cu_up_index_t::invalid,
               "ue={}: could not find a CU-UP to serve the UE",
               request.ue_index);

  return launch_async<pdu_session_resource_setup_routine>(
      request,
      ue_mng.get_ue_config(),
      ue->get_security_manager().get_up_as_config(),
      cfg.security.default_security_indication,
      cu_up_db.find_cu_up_processor(ue->get_cu_up_index())->get_e1ap_bearer_context_manager(),
      du_db.get_du_processor(ue->get_du_index()).get_f1ap_handler(),
      ue->get_rrc_ue(),
      get_cu_cp_rrc_ue_interface(),
      get_cu_cp_mobility_manager_handler(),
      ue->get_task_sched(),
      ue->get_up_resource_manager(),
      logger);
}

async_task<cu_cp_pdu_session_resource_modify_response>
cu_cp_impl::handle_new_pdu_session_resource_modify_request(const cu_cp_pdu_session_resource_modify_request& request)
{
  cu_cp_ue* ue = ue_mng.find_du_ue(request.ue_index);
  ocudu_assert(ue != nullptr, "ue={}: Could not find DU UE", request.ue_index);
  ocudu_assert(
      ue->get_cu_up_index() != cu_up_index_t::invalid, "ue={}: could not find CU-UP of the UE", request.ue_index);

  return launch_async<pdu_session_resource_modification_routine>(
      request,
      cu_up_db.find_cu_up_processor(ue->get_cu_up_index())->get_e1ap_bearer_context_manager(),
      du_db.get_du_processor(ue->get_du_index()).get_f1ap_handler(),
      ue->get_rrc_ue(),
      get_cu_cp_rrc_ue_interface(),
      get_cu_cp_mobility_manager_handler(),
      ue->get_task_sched(),
      ue->get_up_resource_manager(),
      logger);
}

async_task<cu_cp_pdu_session_resource_release_response>
cu_cp_impl::handle_new_pdu_session_resource_release_command(const cu_cp_pdu_session_resource_release_command& command)
{
  cu_cp_ue* ue = ue_mng.find_du_ue(command.ue_index);
  ocudu_assert(ue != nullptr, "ue={}: Could not find DU UE", command.ue_index);
  ocudu_assert(
      ue->get_cu_up_index() != cu_up_index_t::invalid, "ue={}: could not find CU-UP of the UE", command.ue_index);

  return launch_async<pdu_session_resource_release_routine>(
      command,
      cu_up_db.find_cu_up_processor(ue->get_cu_up_index())->get_e1ap_bearer_context_manager(),
      du_db.get_du_processor(ue->get_du_index()).get_f1ap_handler(),
      ue->get_rrc_ue(),
      get_cu_cp_rrc_ue_interface(),
      ue->get_task_sched(),
      ue->get_up_resource_manager(),
      logger);
}

async_task<cu_cp_ue_context_release_complete>
cu_cp_impl::handle_ue_context_release_command(const cu_cp_ue_context_release_command& command)
{
  cu_cp_ue* ue = ue_mng.find_du_ue(command.ue_index);
  ocudu_assert(ue != nullptr, "ue={}: Could not find DU UE", command.ue_index);

  e1ap_bearer_context_manager* e1ap_bearer_ctxt_mng = nullptr;
  if (ue->get_cu_up_index() != cu_up_index_t::invalid) {
    e1ap_bearer_ctxt_mng = &cu_up_db.find_cu_up_processor(ue->get_cu_up_index())->get_e1ap_bearer_context_manager();
  }

  return launch_async<ue_context_release_routine>(command,
                                                  e1ap_bearer_ctxt_mng,
                                                  du_db.get_du_processor(ue->get_du_index()).get_f1ap_handler(),
                                                  get_cu_cp_ue_removal_handler(),
                                                  ue_mng,
                                                  logger);
}

async_task<ngap_handover_resource_allocation_response>
cu_cp_impl::handle_ngap_handover_request(const ngap_handover_request& request)
{
  cu_cp_ue* ue = ue_mng.find_du_ue(request.ue_index);
  ocudu_assert(ue != nullptr, "ue={}: Could not find DU UE", request.ue_index);

  // Select a CU-UP to serve the UE.
  ue->set_cu_up_index(cu_up_db.select_cu_up());
  ocudu_assert(ue->get_cu_up_index() != cu_up_index_t::invalid,
               "ue={}: could not find a CU-UP to serve the UE",
               request.ue_index);

  return launch_async<inter_cu_handover_target_routine>(
      request,
      cu_up_db.find_cu_up_processor(ue->get_cu_up_index())->get_e1ap_bearer_context_manager(),
      du_db.get_du_processor(ue->get_du_index()).get_f1ap_handler(),
      get_cu_cp_ue_removal_handler(),
      ue_mng,
      cell_meas_mng,
      cfg.security.default_security_indication,
      logger);
}

void cu_cp_impl::handle_n2_handover_execution(ue_index_t ue_index)
{
  cu_cp_ue* ue = ue_mng.find_du_ue(ue_index);
  ocudu_assert(ue != nullptr, "ue={}: Could not find DU UE", ue_index);
  ocudu_assert(cu_up_db.find_cu_up_processor(uint_to_cu_up_index(0)) != nullptr,
               "cu_up_index={}: could not find CU-UP",
               uint_to_cu_up_index(0));

  ngap_interface* ngap = ngap_db.find_ngap(ue->get_ue_context().plmn);
  if (ngap == nullptr) {
    logger.warning("ue={}: NGAP not found for PLMN={}", ue_index, ue->get_ue_context().plmn);
    return;
  }

  cu_up_index_t    cu_up_index = uint_to_cu_up_index(0); // TODO: Update when mapping from UE index to CU-UP exists
  cu_up_processor* cu_up       = cu_up_db.find_cu_up_processor(cu_up_index);
  if (cu_up == nullptr) {
    logger.warning("ue={}: could not find CU-UP for handover execution. cu_up={}", ue_index, cu_up_index);
    return;
  }
  e1ap_bearer_context_manager& e1ap = cu_up->get_e1ap_bearer_context_manager();

  ue->get_task_sched().schedule_async_task(
      launch_async<inter_cu_handover_execution_target_routine>(ue, e1ap, *ngap, logger));
}

void cu_cp_impl::handle_transmission_of_handover_required()
{
  // Notify mobility manager metrics handler about the requested handover preparation.
  mobility_mng.get_metrics_handler().aggregate_requested_handover_preparation();
}

async_task<bool> cu_cp_impl::handle_new_handover_command(ue_index_t ue_index, byte_buffer command)
{
  // Notify mobility manager metrics handler about the successful handover preparation.
  mobility_mng.get_metrics_handler().aggregate_successful_handover_preparation();

  cu_cp_ue* ue = ue_mng.find_du_ue(ue_index);
  if (ue == nullptr) {
    logger.warning("ue={}: UE not found for handover command handling", ue_index);
    return launch_async([](coro_context<async_task<bool>>& ctx) {
      CORO_BEGIN(ctx);
      CORO_RETURN(false);
    });
  }
  ngap_interface* ngap = ngap_db.find_ngap(ue->get_ue_context().plmn);
  if (ngap == nullptr) {
    logger.warning("ue={}: NGAP not found for PLMN={}", ue_index, ue->get_ue_context().plmn);
    return launch_async([](coro_context<async_task<bool>>& ctx) {
      CORO_BEGIN(ctx);
      CORO_RETURN(false);
    });
  }
  return launch_async<inter_cu_handover_source_routine>(
      ue_index, std::move(command), ue_mng, du_db, cu_up_db, ngap->get_ngap_control_message_handler(), logger);
}

ue_index_t cu_cp_impl::handle_ue_index_allocation_request(const nr_cell_global_id_t& cgi, const plmn_identity& plmn)
{
  du_index_t du_index = du_db.find_du(cgi);
  if (du_index == du_index_t::invalid) {
    logger.warning("Could not find DU for CGI={}", cgi.nci);
    return ue_index_t::invalid;
  }

  ue_index_t ue_index = ue_mng.add_ue(du_index);
  if (ue_index == ue_index_t::invalid) {
    logger.warning("Could not allocate new UE index for CGI={}", cgi.nci);
    return ue_index_t::invalid;
  }

  if (!handle_ue_plmn_selected(ue_index, plmn)) {
    logger.warning("ue={}: PLMN selection failed", ue_index);
    ue_mng.remove_ue(ue_index);
    return ue_index_t::invalid;
  }

  return ue_index;
}

void cu_cp_impl::handle_dl_ue_associated_nrppa_transport_pdu(ue_index_t ue_index, const byte_buffer& nrppa_pdu)
{
  nrppa_entity->get_nrppa_message_handler().handle_new_nrppa_pdu(nrppa_pdu, ue_index);
}

void cu_cp_impl::handle_dl_non_ue_associated_nrppa_transport_pdu(amf_index_t amf_index, const byte_buffer& nrppa_pdu)
{
  nrppa_entity->get_nrppa_message_handler().handle_new_nrppa_pdu(nrppa_pdu, amf_index);
}

void cu_cp_impl::handle_location_reporting_control_message(ue_index_t ue_index, const ngap_location_report_request& msg)
{
  auto* ue = ue_mng.find_du_ue(ue_index);
  if (ue == nullptr) {
    logger.warning("ue={}: UE not found for location reporting control", ue_index);
    return;
  }

  using event_type = ngap_location_report_request::event_type;

  // Reject events with "nulltype" and send Location Reporting Failure Indication.
  if (msg.location_reporting_type == event_type::nulltype) {
    logger.error("ue={}: received NGAP Location Reporting Control message with nulltype event type, rejecting",
                 ue_index);
    auto* ngap = ngap_db.find_ngap(ue->get_ue_context().plmn);
    if (ngap == nullptr) {
      logger.warning("ue={}: NGAP not found for PLMN={}", ue_index, ue->get_ue_context().plmn);
      return;
    }
    ngap->handle_location_reporting_failure_indication_transmission(
        {ue_index, cause_protocol_t::abstract_syntax_error_falsely_constructed_msg});
    return;
  }

  // Configure the location manager for all report types beside "direct".
  // Send Location Reporting Failure Indication if configuration failed.
  if (msg.location_reporting_type != event_type::direct) {
    auto failure_cause = ue->get_location_manager().configure_location_reporting(msg);
    if (failure_cause.has_value()) {
      auto* ngap = ngap_db.find_ngap(ue->get_ue_context().plmn);
      if (ngap == nullptr) {
        logger.warning("ue={}: NGAP not found for PLMN={}", ue_index, ue->get_ue_context().plmn);
        return;
      }
      ngap->handle_location_reporting_failure_indication_transmission({ue_index, failure_cause.value()});
      return;
    }
  }

  // Send immediate location report if required, 3GPP TS 38.413 8.12.1.2 states that "if reporting upon change of
  // serving cell is requested, the NG-RAN node shall send a report immediately"
  if (msg.location_reporting_type == event_type::direct ||
      msg.location_reporting_type == event_type::change_of_serve_cell ||
      msg.location_reporting_type == event_type::change_of_serving_cell_and_ue_presence_in_the_area_of_interest) {
    // Get cell info and build location report immediately.
    const auto* cell = du_db.get_du_processor(ue->get_du_index()).get_context()->find_cell(ue->get_pci());
    if (cell == nullptr) {
      logger.warning("ue={}: Cell not found for PCI={}", ue_index, ue->get_pci());
      return;
    }

    cu_cp_user_location_info_nr user_location_info;
    user_location_info.nr_cgi = cell->cgi;
    user_location_info.tai    = {cell->cgi.plmn_id, cell->tac};
    auto report = ue->get_location_manager().get_direct_location_report(ue_index, user_location_info, msg);

    auto* ngap = ngap_db.find_ngap(ue->get_ue_context().plmn);
    if (ngap == nullptr) {
      logger.warning("ue={}: NGAP not found for PLMN={}", ue_index, ue->get_ue_context().plmn);
      return;
    }

    ngap->handle_location_report_transmission(report);
  }
}

void cu_cp_impl::handle_location_update(ue_index_t ue_index)
{
  auto* ue = ue_mng.find_du_ue(ue_index);
  if (ue == nullptr) {
    logger.warning("ue={}: UE not found for cell change location report", ue_index);
    return;
  }

  const auto* cell = du_db.get_du_processor(ue->get_du_index()).get_context()->find_cell(ue->get_pci());
  if (cell == nullptr) {
    logger.warning("ue={}: Cell not found for PCI={}", ue_index, ue->get_pci());
    return;
  }

  cu_cp_user_location_info_nr user_location_info;
  user_location_info.nr_cgi = cell->cgi;
  user_location_info.tai    = {cell->cgi.plmn_id, cell->tac};

  auto opt_report = ue->get_location_manager().get_location_report(ue_index, user_location_info);
  if (!opt_report.has_value()) {
    return;
  }

  auto* ngap = ngap_db.find_ngap(ue->get_ue_context().plmn);
  if (ngap == nullptr) {
    logger.warning("ue={}: NGAP not found for PLMN={}", ue_index, ue->get_ue_context().plmn);
    return;
  }

  ngap->handle_location_report_transmission(opt_report.value());
}

nrppa_cu_cp_ue_notifier* cu_cp_impl::handle_new_nrppa_ue(ue_index_t ue_index)
{
  auto* ue = ue_mng.find_ue(ue_index);
  if (ue == nullptr) {
    return nullptr;
  }
  return &ue->get_nrppa_cu_cp_ue_notifier();
}

void cu_cp_impl::handle_ul_nrppa_pdu(const byte_buffer&                    nrppa_pdu,
                                     std::variant<ue_index_t, amf_index_t> ue_or_amf_index)
{
  if (std::holds_alternative<ue_index_t>(ue_or_amf_index)) {
    ue_index_t ue_index = std::get<ue_index_t>(ue_or_amf_index);

    if (ue_mng.find_du_ue(ue_index) == nullptr) {
      logger.warning("UE index={} got removed", ue_index);
      return;
    }

    auto* ue = ue_mng.find_du_ue(ue_index);

    auto* ngap = ngap_db.find_ngap(ue->get_ue_context().plmn);
    if (ngap == nullptr) {
      logger.warning("NGAP not found for PLMN={}", ue->get_ue_context().plmn);
      return;
    }

    // Forward the NRPPa message to the NGAP.
    ngap->handle_ul_ue_associated_nrppa_transport(ue_index, nrppa_pdu);
  } else {
    amf_index_t amf_index = std::get<amf_index_t>(ue_or_amf_index);

    // Forward the NRPPa message to the NGAP.
    common_task_sched.schedule_async_task(
        launch_async([this, amf_index, nrppa_pdu](coro_context<async_task<void>>& ctx) {
          CORO_BEGIN(ctx);

          if (ngap_db.find_ngap(amf_index) == nullptr) {
            logger.warning("NGAP not found for AMF index={}", amf_index);
            CORO_EARLY_RETURN();
          }

          CORO_AWAIT(ngap_db.find_ngap(amf_index)->handle_ul_non_ue_associated_nrppa_transport(nrppa_pdu));
          CORO_RETURN();
        }));
  }
}

async_task<trp_information_cu_cp_response_t>
cu_cp_impl::handle_trp_information_request(const trp_information_request_t& request)
{
  return launch_async<trp_information_exchange_routine>(request, du_db, nrppa_f1ap_ev_notifiers);
}

void cu_cp_impl::handle_n2_disconnection(amf_index_t amf_index)
{
  std::vector<plmn_identity> plmns = ngap_db.find_ngap(amf_index)->get_ngap_context().get_supported_plmns();

  logger.warning("Handling N2 disconnection. Lost PLMNs: {}", fmt::format("{}", fmt::join(plmns, " ")));

  common_task_sched.schedule_async_task(
      launch_async<amf_connection_loss_routine>(amf_index, cfg, plmns, du_db, *this, ue_mng, controller, logger));
}

std::optional<rrc_meas_cfg>
cu_cp_impl::handle_measurement_config_request(ue_index_t                         ue_index,
                                              nr_cell_identity                   nci,
                                              const std::optional<rrc_meas_cfg>& current_meas_config,
                                              bool                               cond_meas,
                                              span<const pci_t>                  candidate_pcis)
{
  return cell_meas_mng.get_measurement_config(ue_index, nci, current_meas_config, cond_meas, candidate_pcis);
}

void cu_cp_impl::handle_measurement_report(ue_index_t ue_index, const rrc_meas_results& meas_results)
{
  cell_meas_mng.report_measurement(ue_index, meas_results);
}

bool cu_cp_impl::handle_cell_config_update_request(nr_cell_identity nci, const serving_cell_meas_config& serv_cell_cfg)
{
  return cell_meas_mng.update_cell_config(nci, serv_cell_cfg);
}

async_task<cu_cp_intra_cu_handover_response>
cu_cp_impl::handle_intra_cu_handover_request(const cu_cp_intra_cu_handover_request& request,
                                             du_index_t&                            source_du_index,
                                             du_index_t&                            target_du_index)
{
  cu_cp_ue* ue = ue_mng.find_du_ue(request.source_ue_index);
  ocudu_assert(ue != nullptr, "ue={}: Could not find DU UE", request.source_ue_index);

  byte_buffer sib1 = du_db.get_du_processor(target_du_index).get_mobility_handler().get_packed_sib1(request.cgi);

  return launch_async<intra_cu_handover_routine>(request,
                                                 std::move(sib1),
                                                 du_db.get_du_processor(source_du_index).get_f1ap_handler(),
                                                 du_db.get_du_processor(target_du_index).get_f1ap_handler(),
                                                 *this,
                                                 ue_mng,
                                                 mobility_mng,
                                                 logger);
}

async_task<cu_cp_intra_cu_cho_response>
cu_cp_impl::handle_intra_cu_cho_request(const cu_cp_intra_cu_cho_request& request)
{
  if (request.source_du_index == du_index_t::invalid) {
    logger.warning("ue={}: Invalid source DU index for intra-CU CHO coordinator", request.source_ue_index);
    return launch_async([](coro_context<async_task<cu_cp_intra_cu_cho_response>>& ctx) {
      CORO_BEGIN(ctx);
      CORO_RETURN(cu_cp_intra_cu_cho_response{});
    });
  }
  return launch_async<cho_coordinator_routine>(request, du_db, *this, ue_mng, mobility_mng, logger);
}

void cu_cp_impl::handle_intra_cell_handover_required(ue_index_t ue_index)
{
  cu_cp_ue* ue = ue_mng.find_du_ue(ue_index);
  ocudu_assert(ue != nullptr, "ue={}: Could not find DU UE", ue_index);

  if (ue != nullptr) {
    nr_cell_global_id_t cgi = du_db.get_du_processor(ue->get_du_index()).get_context()->find_cell(ue->get_pci())->cgi;
    cu_cp_intra_cu_handover_request intra_cu_handover_request = {ue_index, ue->get_du_index(), cgi, ue->get_pci()};

    byte_buffer sib1 = du_db.get_du_processor(ue->get_du_index())
                           .get_mobility_handler()
                           .get_packed_sib1(intra_cu_handover_request.cgi);
    auto& du_processor = du_db.get_du_processor(ue->get_du_index()).get_f1ap_handler();
    ue->get_task_sched().schedule_async_task(launch_async([this, intra_cu_handover_request, &du_processor, sib1](
                                                              coro_context<async_task<void>>& ctx) mutable {
      CORO_BEGIN(ctx);

      CORO_AWAIT(launch_async<intra_cu_handover_routine>(
          intra_cu_handover_request, std::move(sib1), du_processor, du_processor, *this, ue_mng, mobility_mng, logger));

      CORO_RETURN();
    }));
  }
}

async_task<void> cu_cp_impl::handle_ue_removal_request(ue_index_t ue_index)
{
  if (ue_mng.find_du_ue(ue_index) == nullptr) {
    logger.warning("ue={}: Could not find DU UE", ue_index);
    return launch_async([](coro_context<async_task<void>>& ctx) {
      CORO_BEGIN(ctx);
      CORO_RETURN();
    });
  }
  auto* ue = ue_mng.find_ue(ue_index);

  du_index_t    du_index    = ue->get_du_index();
  cu_up_index_t cu_up_index = ue->get_cu_up_index();

  e1ap_bearer_context_removal_handler* e1ap_removal_handler = nullptr;
  if (cu_up_index != cu_up_index_t::invalid) {
    e1ap_removal_handler = &cu_up_db.find_cu_up_processor(cu_up_index)->get_e1ap_bearer_context_removal_handler();
  }

  rrc_ue_handler*                  rrc_ue_removal_handler = nullptr;
  f1ap_ue_context_removal_handler* f1ap_removal_handler   = nullptr;
  if (du_index != du_index_t::invalid) {
    du_processor* du_proc = du_db.find_du_processor(du_index);
    if (du_proc != nullptr) {
      rrc_ue_removal_handler = &du_proc->get_rrc_du_handler();
      f1ap_removal_handler   = &du_proc->get_f1ap_handler();
    }
  }

  auto*                            ngap                 = ngap_db.find_ngap(ue->get_ue_context().plmn);
  ngap_ue_context_removal_handler* ngap_removal_handler = nullptr;
  if (ngap != nullptr) {
    ngap_removal_handler = &ngap->get_ngap_ue_context_removal_handler();
  }

  nrppa_ue_context_removal_handler* nrppa_removal_handler = nullptr;
  nrppa_removal_handler                                   = &nrppa_entity->get_nrppa_ue_context_removal_handler();

  return launch_async<ue_removal_routine>(ue_index,
                                          rrc_ue_removal_handler,
                                          e1ap_removal_handler,
                                          f1ap_removal_handler,
                                          ngap_removal_handler,
                                          nrppa_removal_handler,
                                          ue_mng,
                                          logger);
}

void cu_cp_impl::handle_pending_ue_task_cancellation(ue_index_t ue_index)
{
  ocudu_assert(ue_mng.find_du_ue(ue_index) != nullptr, "ue={}: Could not find DU UE", ue_index);

  // Clear all enqueued tasks for this UE.
  ue_mng.get_task_sched().clear_pending_tasks(ue_index);

  // Cancel running transactions for the RRC UE.
  rrc_ue_interface* rrc_ue = ue_mng.find_du_ue(ue_index)->get_rrc_ue();
  if (rrc_ue != nullptr) {
    rrc_ue->get_controller().stop();
  }
}

void cu_cp_impl::handle_amf_reconnection(amf_index_t amf_index)
{
  if (ngap_db.find_ngap(amf_index) == nullptr) {
    logger.warning("AMF index={} not found", amf_index);
    return;
  }

  std::vector<plmn_identity> served_plmns = ngap_db.find_ngap(amf_index)->get_ngap_context().get_supported_plmns();

  common_task_sched.schedule_async_task(launch_async<cell_activation_routine>(cfg, served_plmns, du_db, logger));
}

void cu_cp_impl::initialize_handover_ue_release_timer(
    ue_index_t                              ue_index,
    std::chrono::milliseconds               handover_ue_release_timeout,
    const cu_cp_ue_context_release_request& ue_context_release_request)
{
  if (ue_mng.find_du_ue(ue_index) == nullptr) {
    logger.warning("ue={}: Could not find UE", ue_index);
    return;
  }

  cu_cp_ue* ue = ue_mng.find_ue(ue_index);

  if (ue->get_handover_ue_release_timer().is_running()) {
    logger.warning("ue={}: handover UE release timer already running", ue_index);
    return;
  }

  // Start timer.
  logger.debug("ue={}: Setting release timer to {}ms", ue_index, handover_ue_release_timeout.count());
  ue->get_handover_ue_release_timer().set(
      handover_ue_release_timeout, [this, ue, ue_context_release_request](timer_id_t /*tid*/) {
        ue->get_task_sched().schedule_async_task(handle_ue_context_release(ue_context_release_request));
      });
  ue->get_handover_ue_release_timer().run();
}

void cu_cp_impl::initialize_rna_update_timer(ue_index_t ue_index)
{
  if (ue_mng.find_du_ue(ue_index) == nullptr) {
    logger.warning("ue={}: Could not find UE", ue_index);
    return;
  }

  cu_cp_ue* ue = ue_mng.find_ue(ue_index);

  if (ue->get_rna_update_timer().is_running()) {
    logger.warning("ue={}: RNA update timer already running", ue_index);
    return;
  }

  // Start RNA update timer (T380). When this timer expires, the UE will be released if it hasn't initiated RRC Resume
  // procedure to send data or send mandatory RNA update.
  // NOTE: A guard time is added to leave time for the resume message to be received.
  std::chrono::seconds rna_guard_time{1};
  logger.debug("ue={}: Setting release timer to {}min (+ {}s guard time)",
               ue_index,
               cfg.ue.t380.count(),
               rna_guard_time.count());
  unique_timer& rna_update_timer = ue->get_rna_update_timer();
  rna_update_timer.set(cfg.ue.t380 + rna_guard_time,
                       [this, ue_idx = ue_index](timer_id_t /*tid*/) { request_release_of_inactive_ue(ue_idx); });
  rna_update_timer.run();
}

void cu_cp_impl::initialize_cho_execution_timer(ue_index_t ue_index, std::chrono::milliseconds timeout)
{
  if (timeout.count() == 0) {
    return;
  }

  cu_cp_ue* ue = ue_mng.find_ue(ue_index);
  if (ue == nullptr || !ue->get_cho_context().has_value()) {
    return;
  }

  if (ue->get_cho_context()->cho_execution_timer.is_running()) {
    logger.warning("ue={}: CHO execution timer already running", ue_index);
    return;
  }

  ue->get_cho_context()->cho_execution_timer = ue->get_task_sched().create_timer();
  ue->get_cho_context()->cho_execution_timer.set(timeout, [this, ue_index](timer_id_t /*tid*/) {
    cu_cp_ue* ue2 = ue_mng.find_du_ue(ue_index);
    if (ue2 == nullptr || !ue2->get_cho_context().has_value()) {
      return;
    }
    ue2->get_task_sched().schedule_async_task(launch_async<cho_cancellation_routine>(ue_index, *this, ue_mng, logger));
  });
  ue->get_cho_context()->cho_execution_timer.run();
  logger.debug("ue={}: CHO execution timer started ({}ms)", ue_index, timeout.count());
}

// private

void cu_cp_impl::handle_rrc_ue_creation(ue_index_t ue_index, rrc_ue_interface& rrc_ue)
{
  // Store the RRC UE in the UE manager.
  auto* ue = ue_mng.find_ue(ue_index);
  ue->set_rrc_ue(rrc_ue);

  // Connect RRC UE to NGAP to RRC UE adapter.
  ue_mng.get_ngap_rrc_ue_adapter(ue_index).connect_rrc_ue(rrc_ue.get_rrc_ngap_message_handler());

  // Connect CU-CP to RRC UE adapter.
  ue_mng.get_rrc_ue_cu_cp_adapter(ue_index).connect_cu_cp(get_cu_cp_rrc_ue_interface(),
                                                          get_cu_cp_ue_removal_handler(),
                                                          controller,
                                                          ue->get_up_resource_manager(),
                                                          get_cu_cp_measurement_handler());
}

byte_buffer cu_cp_impl::handle_target_cell_sib1_required(du_index_t du_index, nr_cell_global_id_t cgi)
{
  return du_db.get_du_processor(du_index).get_mobility_handler().get_packed_sib1(cgi);
}

async_task<void> cu_cp_impl::handle_transaction_info_loss(const ue_transaction_info_loss_event& ev)
{
  return launch_async<ue_transaction_info_release_routine>(ev, ue_mng, ngap_db, cu_up_db, *this, logger);
}

ngap_cu_cp_ue_notifier* cu_cp_impl::handle_new_ngap_ue(ue_index_t ue_index)
{
  auto* ue = ue_mng.find_ue(ue_index);
  if (ue == nullptr) {
    return nullptr;
  }
  return &ue->get_ngap_cu_cp_ue_notifier();
}

bool cu_cp_impl::schedule_ue_task(ue_index_t ue_index, async_task<void> task)
{
  if (ue_mng.find_ue_task_scheduler(ue_index) == nullptr) {
    logger.debug("UE task scheduler not found for UE index={}", ue_index);
    return false;
  }

  return ue_mng.find_ue_task_scheduler(ue_index)->schedule_async_task(std::move(task));
}

void cu_cp_impl::request_ue_release(cu_cp_ue& ue, const ngap_cause_t& cause)
{
  cu_cp_ue_context_release_request req;
  req.ue_index = ue.get_ue_index();
  req.cause    = cause;

  // Add PDU Session IDs.
  auto& up_resource_manager            = ue.get_up_resource_manager();
  req.pdu_session_res_list_cxt_rel_req = up_resource_manager.get_pdu_sessions();

  logger.debug("ue={}: Requesting UE context release with cause={}", req.ue_index, req.cause);

  // Schedule on UE task scheduler.
  ue.get_task_sched().schedule_async_task(launch_async([this, req](coro_context<async_task<void>>& ctx) mutable {
    CORO_BEGIN(ctx);
    // Notify NGAP to request a release from the AMF.
    CORO_AWAIT(handle_ue_context_release(req));
    CORO_RETURN();
  }));
}

void cu_cp_impl::send_ran_paging(ue_index_t ue_index, full_i_rnti_t full_i_rnti)
{
  cu_cp_ue* ue = ue_mng.find_du_ue(ue_index);
  if (ue == nullptr) {
    logger.warning("ue={}: Can't initiate RAN paging. UE not found", ue_index);
    return;
  }

  // Check if RAN paging timer is already running and drop the new paging request if needed.
  if (ue->get_ran_paging_timer().is_running()) {
    logger.debug("ue={}: RAN paging timer already running, dropping new paging request", ue_index);
    return;
  }

  auto* ngap = ngap_db.find_ngap(ue->get_ue_context().plmn);
  if (ngap == nullptr) {
    logger.warning("ue={}: Can't initiate RAN paging. NGAP not found for plmn={}", ue_index, ue->get_ue_context().plmn);
    return;
  }

  // Get Core Network Assist Info for Inactive if present.
  std::optional<ngap_core_network_assist_info_for_inactive> cn_assist_info_for_inactive =
      ngap->get_cn_assist_info_for_inactive(ue_index);

  if (!cn_assist_info_for_inactive.has_value()) {
    logger.warning("ue={}: Can't initiate RAN paging. Core Network Assist Info for Inactive not available", ue_index);
    return;
  }

  // Fill paging message.
  cu_cp_paging_message cu_cp_paging_msg;

  cu_cp_paging_msg.ue_id_idx_value = cn_assist_info_for_inactive->ue_id_idx_value;
  cu_cp_paging_msg.paging_drx      = cn_assist_info_for_inactive->ue_specific_drx;
  cu_cp_paging_msg.ue_paging_id    = full_i_rnti;

  if (!cu_cp_paging_msg.paging_drx.has_value()) {
    // Use RAN configured paging DRX if not available from CN assist info.
    cu_cp_paging_msg.paging_drx = cfg.ue.ran_paging_cycle;
  }

  cu_cp_paging_msg.assist_data_for_paging.emplace();
  cu_cp_paging_msg.assist_data_for_paging->assist_data_for_recommended_cells.emplace();

  du_processor& du_proc = du_db.get_du_processor(ue->get_du_index());

  // Add recommended cells for paging.
  // Note: Currently only RAN area cells are supported.
  for (const auto& ran_area_cell : du_proc.get_rrc_du_handler().get_ran_area_cells()) {
    if (!ran_area_cell.plmn_id.has_value()) {
      continue;
    }

    for (const auto& cell_id : ran_area_cell.ran_area_cells) {
      nr_cell_global_id_t nr_cgi(ran_area_cell.plmn_id.value(), cell_id);

      cu_cp_paging_msg.assist_data_for_paging->assist_data_for_recommended_cells->recommended_cells_for_paging
          .recommended_cell_list.push_back(cu_cp_recommended_cell_item{.ngran_cgi = nr_cgi});
    }
  }

  // Send paging message.
  paging_handler.handle_paging_message(cu_cp_paging_msg);

  // Start RAN paging timer. When this timer expires, the UE will be released if it has not been resumed yet.
  unique_timer& ran_paging_timer = ue->get_ran_paging_timer();

  // Calculate RAN paging timeout based on the paging (e)DRX and the number of paging cycles. A guard time of 1 hyper
  // frame is added to ensure that the timer expires after the last paging occasion. If paging eDRX is not available,
  // the timer is set to 1 hyper frame.
  std::chrono::seconds ran_paging_timeout = std::chrono::duration_cast<std::chrono::seconds>(
      hyper_frames{1} + (cu_cp_paging_msg.paging_edrx_info.has_value()
                             ? cu_cp_paging_msg.paging_edrx_info->nr_paging_edrx_cycle * hyper_frames{1}
                             : hyper_frames{0}));
  ran_paging_timer.set(ran_paging_timeout,
                       [this, ue_idx = ue_index](timer_id_t /*tid*/) { request_release_of_inactive_ue(ue_idx); });
  ran_paging_timer.run();
}

void cu_cp_impl::request_release_of_inactive_ue(ue_index_t ue_index)
{
  cu_cp_ue* ue = ue_mng.find_ue(ue_index);
  if (ue == nullptr) {
    logger.debug("ue={}: Could not find UE", ue_index);
    return;
  }

  request_ue_release(*ue, ngap_cause_radio_network_t::ue_in_rrc_inactive_state_not_reachable);
}

void cu_cp_impl::on_statistics_report_timer_expired()
{
  // Get number of F1AP UEs.
  unsigned nof_f1ap_ues = du_db.get_nof_f1ap_ues();

  // Get number of RRC UEs.
  unsigned nof_rrc_ues = du_db.get_nof_rrc_ues();

  // Get number of NGAP UEs.
  unsigned nof_ngap_ues = ngap_db.get_nof_ngap_ues();

  // Get number of E1AP UEs.
  unsigned nof_e1ap_ues = cu_up_db.get_nof_e1ap_ues();

  // Get number of CU-CP UEs.
  unsigned nof_cu_cp_ues = ue_mng.get_nof_ues();

  // Log statistics.
  logger.debug("num_f1ap_ues={} num_rrc_ues={} num_ngap_ues={} num_e1ap_ues={} num_cu_cp_ues={}",
               nof_f1ap_ues,
               nof_rrc_ues,
               nof_ngap_ues,
               nof_e1ap_ues,
               nof_cu_cp_ues);

  // Restart timer.
  statistics_report_timer.set(cfg.metrics.statistics_report_period,
                              [this](timer_id_t /*tid*/) { on_statistics_report_timer_expired(); });
  statistics_report_timer.run();
}
