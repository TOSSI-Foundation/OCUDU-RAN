// SPDX-FileCopyrightText: Copyright (C) 2021-2026 Software Radio Systems Limited
// SPDX-License-Identifier: BSD-3-Clause-Open-MPI
// Portions of this file may implement 3GPP specifications, which may be subject to additional licensing requirements.

#include "du_manager_impl.h"
#include "du_positioning_handler_factory.h"
#include "procedures/cu_configuration_procedure.h"
#include "procedures/du_bsr_periodicity_actuation_procedure.h"
#include "procedures/du_cell_stop_procedure.h"
#include "procedures/du_mac_ntn_param_update_procedure.h"
#include "procedures/du_param_config_procedure.h"
#include "procedures/du_setup_procedure.h"
#include "procedures/du_stop_procedure.h"
#include "procedures/du_ue_reset_procedure.h"
#include "procedures/du_ue_ric_configuration_procedure.h"
#include "procedures/f1c_disconnection_handling_procedure.h"
#include "ocudu/mac/mac_pdu_handler.h"
#include "ocudu/support/async/async_timer.h"
#include "ocudu/support/async/execute_on_blocking.h"
#include "ocudu/support/executors/execute_until_success.h"
#include "ocudu/support/synchronization/sync_event.h"
#include <thread>

using namespace ocudu;
using namespace odu;

du_manager_impl::du_manager_impl(const du_manager_params& params_) :
  params(params_),
  logger(ocudulog::fetch_basic_logger("DU-MNG")),
  cell_mng(params),
  cell_res_alloc(params.ran.cells, params.mac.sched_cfg, params.ran.srbs, params.ran.qos, params.test_cfg),
  ue_mng(params, cell_res_alloc, cell_mng),
  positioning_handler(create_du_positioning_handler(params, cell_mng, ue_mng, logger)),
  metrics(params.metrics, params.services.du_mng_exec, params.services.timers, params.f1ap.metrics),
  proc_ctxt{params, ctxt, cell_mng, ue_mng, metrics, logger},
  main_ctrl_loop(128)
{
  const bool actuation_enabled =
      not params.ran.cells.empty() and params.ran.cells.front().bsr_ml_actuation_enabled;
  if (actuation_enabled) {
    metrics.set_bsr_periodicity_recommendation_handler(
        [this](du_ue_index_t ue_index, unsigned recommended_sf) {
          handle_bsr_periodicity_recommendation(ue_index, recommended_sf);
        });
    logger.info("BSR periodicity ML actuation enabled (F1AP UE Context Modification Required, TS 38.473 §8.3.5).");
  }
}

namespace {

// TS 38.331
constexpr unsigned VALID_PERIODIC_BSR_TIMER_SF[] = {1, 5, 10, 16, 20, 32, 40, 64, 80, 128, 160, 320, 640, 1280, 2560};

int periodicity_index(unsigned sf)
{
  for (int i = 0; i < static_cast<int>(sizeof(VALID_PERIODIC_BSR_TIMER_SF) / sizeof(unsigned)); ++i) {
    if (VALID_PERIODIC_BSR_TIMER_SF[i] == sf) {
      return i;
    }
  }
  return -1;
}

constexpr unsigned BSR_ACT_HYSTERESIS_STEPS = 1;
constexpr unsigned BSR_ACT_MIN_DWELL_SEC    = 10;

}

void du_manager_impl::handle_bsr_periodicity_recommendation(du_ue_index_t ue_index, unsigned recommended_sf)
{
  bsr_actuation_state& st = bsr_actuation_states[ue_index];

  if (st.applied_sf == recommended_sf) {
    return;
  }

  if (st.applied_sf != 0) {
    const int cur = periodicity_index(st.applied_sf);
    const int nw  = periodicity_index(recommended_sf);
    if (cur >= 0 and nw >= 0) {
      int dist = nw - cur;
      if (dist < 0) {
        dist = -dist;
      }
      if (static_cast<unsigned>(dist) <= BSR_ACT_HYSTERESIS_STEPS) {
        return;
      }
    }
  }

  const auto now = std::chrono::steady_clock::now();
  if (st.applied_sf != 0 and BSR_ACT_MIN_DWELL_SEC > 0 and
      (now - st.last_tp) < std::chrono::seconds(BSR_ACT_MIN_DWELL_SEC)) {
    return;
  }

  st.applied_sf = recommended_sf;
  st.last_tp    = now;
  ue_mng.schedule_async_task(ue_index, start_bsr_periodicity_actuation(ue_index, recommended_sf, ue_mng, params));
}

du_manager_impl::~du_manager_impl()
{
  stop();
}

void du_manager_impl::start()
{
  {
    std::unique_lock<std::mutex> lock(mutex);
    if (ctxt.running) {
      logger.warning("Ignoring start request. Cause: DU Manager already started.");
      return;
    }
  }

  logger.info("DU manager starting...");

  if (not params.services.du_mng_exec.execute([this]() {
        main_ctrl_loop.schedule([this](coro_context<async_task<void>>& ctx) {
          CORO_BEGIN(ctx);

          // Connect to CU-CP and send F1 Setup Request and await for F1 setup response.
          CORO_AWAIT(launch_async<du_setup_procedure>(proc_ctxt));

          // Signal start() caller thread that the operation is complete.
          std::lock_guard<std::mutex> lock(mutex);
          ctxt.running = true;
          cvar.notify_all();

          CORO_RETURN();
        });
      })) {
    report_fatal_error("Unable to initiate DU setup procedure");
  }

  // Block waiting for DU setup to complete.
  std::unique_lock<std::mutex> lock(mutex);
  cvar.wait(lock, [this]() { return ctxt.running; });

  logger.info("DU manager started successfully.");
}

void du_manager_impl::stop()
{
  {
    // Avoid stopping the DU Manager multiple times.
    std::lock_guard<std::mutex> lock(mutex);
    if (not ctxt.running) {
      return;
    }
  }

  while (not params.services.du_mng_exec.execute([this]() { handle_du_stop_request(); })) {
    logger.error("Unable to dispatch DU Manager shutdown. Retrying...");
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
    return;
  }

  // Wait for the DU Manager thread to signal that the stop was completed.
  std::unique_lock<std::mutex> lock(mutex);
  cvar.wait(lock, [this]() { return not ctxt.running; });
}

void du_manager_impl::handle_ul_ccch_indication(const ul_ccch_indication_message& msg)
{
  // Switch DU Manager exec context
  if (not params.services.du_mng_exec.execute([this, msg = std::move(msg)]() {
        // Start UE create procedure
        ue_mng.handle_ue_create_request(msg);
      })) {
    logger.warning("Discarding UL-CCCH message cell={} tc-rnti={} slot_rx={}. Cause: DU manager task queue is full",
                   fmt::underlying(msg.cell_index),
                   msg.tc_rnti,
                   msg.slot_rx);
  }
}

void du_manager_impl::handle_crnti_ce_indication(const ul_crnti_ce_indication_message& msg)
{
  if (not params.services.du_mng_exec.execute([this, msg]() {
        du_ue* ue = ue_mng.find_ue(msg.ue_index);
        if (ue == nullptr || !cell_mng.has_cell(msg.cell_index)) {
          return;
        }

        const bool reached_prepared_target = ue->cond_mobility.handle_crnti_ce_indication();
        if (not reached_prepared_target) {
          logger.debug("ue={} cell={}: C-RNTI CE received but no Access Success is expected. Ignoring.",
                       fmt::underlying(msg.ue_index),
                       fmt::underlying(msg.cell_index));
          return;
        }

        const auto target_cgi = cell_mng.get_cell_cfg(msg.cell_index).nr_cgi;
        params.f1ap.ue_mng.handle_access_success({msg.ue_index, target_cgi});
      })) {
    logger.warning("cell={} ue={}: Discarding C-RNTI CE indication. Cause: DU manager task queue is full",
                   fmt::underlying(msg.cell_index),
                   fmt::underlying(msg.ue_index));
  }
}

void du_manager_impl::handle_f1c_connection_loss()
{
  schedule_async_task(launch_async<f1c_disconnection_handling_procedure>(proc_ctxt));
}

void du_manager_impl::handle_du_stop_request()
{
  if (not ctxt.running) {
    // Already stopped.
    return;
  }

  // Notify other procedures that the DU needs to stop.
  ctxt.stop_command_received = true;

  // Start DU stop procedure.
  schedule_async_task(launch_async([this](coro_context<async_task<void>>& ctx) {
    CORO_BEGIN(ctx);

    if (not ctxt.running) {
      // Already stopped.
      CORO_EARLY_RETURN();
    }

    // Tear down activity in remaining layers.
    CORO_AWAIT(launch_async<du_stop_procedure>(ue_mng, cell_mng, params.f1ap.conn_mng));

    // DU stop successfully finished.
    // Dispatch main async task loop destruction via defer so that the current coroutine ends successfully.
    while (not params.services.du_mng_exec.defer([this]() {
      // Let main loop go out of scope and be destroyed.
      auto main_loop = main_ctrl_loop.request_stop();

      std::lock_guard<std::mutex> lock(mutex);
      ctxt.running               = false;
      ctxt.stop_command_received = false;
      cvar.notify_all();
    })) {
      logger.warning("Unable to stop DU Manager. Retrying...");
      std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }

    CORO_RETURN();
  }));
}

du_ue_index_t du_manager_impl::find_unused_du_ue_index()
{
  return ue_mng.find_unused_du_ue_index();
}

async_task<void> du_manager_impl::handle_f1_reset_request(const std::vector<du_ue_index_t>& ues_to_reset)
{
  return launch_async<du_ue_reset_procedure>(ues_to_reset, ue_mng, params, std::nullopt);
}

async_task<gnbcu_config_update_response>
du_manager_impl::handle_cu_context_update_request(const gnbcu_config_update_request& request)
{
  return launch_async<cu_configuration_procedure>(request, cell_mng, ue_mng, params, metrics);
}

async_task<f1ap_ue_context_creation_response>
du_manager_impl::handle_ue_context_creation(const f1ap_ue_context_creation_request& request)
{
  return ue_mng.handle_ue_create_request(request);
}

async_task<f1ap_ue_context_update_response>
du_manager_impl::handle_ue_context_update(const f1ap_ue_context_update_request& request)
{
  return ue_mng.handle_ue_config_request(request);
}

async_task<void> du_manager_impl::handle_ue_delete_request(const f1ap_ue_delete_request& request)
{
  return ue_mng.handle_ue_delete_request(request);
}

async_task<void> du_manager_impl::handle_ue_drb_deactivation_request(du_ue_index_t ue_index)
{
  return ue_mng.handle_ue_drb_deactivation_request(ue_index);
}

void du_manager_impl::handle_ue_reestablishment(du_ue_index_t new_ue_index, du_ue_index_t old_ue_index)
{
  ue_mng.handle_reestablishment_request(new_ue_index, old_ue_index);
}

void du_manager_impl::handle_ue_config_applied(du_ue_index_t ue_index)
{
  ue_mng.handle_ue_config_applied(ue_index);
}

size_t du_manager_impl::nof_ues()
{
  // TODO: This is temporary code.
  size_t     result = 0;
  sync_event ev;
  if (not params.services.du_mng_exec.execute(
          [this, &result, tk = ev.get_token()]() mutable { result = ue_mng.nof_ues(); })) {
    logger.warning("Unable to compute the number of UEs active in the DU");
    return std::numeric_limits<size_t>::max();
  }
  return result;
}

mac_subframe_time_mapper& du_manager_impl::get_subframe_time_mapper()
{
  return params.mac.mgr.get_subframe_time_mapper();
}

async_task<du_mac_sched_control_config_response>
du_manager_impl::configure_ue_mac_scheduler(du_mac_sched_control_config reconf)
{
  return launch_async<du_ue_ric_configuration_procedure>(reconf, ue_mng, params);
}

async_task<du_param_config_response> du_manager_impl::handle_operator_config(const du_param_config_request& req,
                                                                             task_executor& continuation_exec)
{
  return launch_async([this, &continuation_exec, req, resp = expected<du_param_config_response>{}](
                          coro_context<async_task<du_param_config_response>>& ctx) mutable {
    CORO_BEGIN(ctx);

    // Move to DU manager execution context.
    CORO_AWAIT(defer_on_blocking(params.services.du_mng_exec, params.services.timers));

    // Launch procedure in the DU manager task scheduler.
    CORO_AWAIT_VALUE(resp,
                     when_coroutine_completed_on_task_sched(
                         main_ctrl_loop, launch_async<du_param_config_procedure>(req, params, cell_mng)));

    // Move back to the caller execution context.
    CORO_AWAIT(defer_on_blocking(continuation_exec, params.services.timers));

    CORO_RETURN(resp.value());
  });
}

du_param_config_response du_manager_impl::handle_sync_operator_config(const du_param_config_request& req)
{
  sync_event               ev;
  du_param_config_response resp;

  // Switch to DU manager execution context.
  execute_until_success(
      params.services.du_mng_exec, params.services.timers, [this, &req, &resp, tk = ev.get_token()]() mutable {
        // Dispatch procedure in DU manager task scheduler.
        // Also, bind the lifetime of the token to the procedure completion.
        schedule_async_task(launch_async([this, &resp, &req, tk = std::move(tk)](coro_context<async_task<void>>& ctx) {
          CORO_BEGIN(ctx);
          // Launch config procedure.
          CORO_AWAIT_VALUE(resp, launch_async<du_param_config_procedure>(req, params, cell_mng));
          CORO_RETURN();
        }));
      });

  // Block waiting for sync event token to go out of scope.
  ev.wait();
  return resp;
}

void du_manager_impl::handle_ntn_param_update(const du_ntn_param_update_request& req)
{
  schedule_async_task(launch_async([&req, this](coro_context<async_task<void>>& ctx) {
    CORO_BEGIN(ctx);

    if (not ctxt.running) {
      // Already stopped.
      CORO_EARLY_RETURN();
    }
    CORO_AWAIT(start_du_ntn_param_update(req, params, cell_mng));

    CORO_RETURN();
  }));
}
