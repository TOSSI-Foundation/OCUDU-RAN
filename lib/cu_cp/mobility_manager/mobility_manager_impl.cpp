// SPDX-FileCopyrightText: Copyright (C) 2021-2026 Software Radio Systems Limited
// SPDX-License-Identifier: BSD-3-Clause-Open-MPI
// Portions of this file may implement 3GPP specifications, which may be subject to additional licensing requirements.

#include "mobility_manager_impl.h"
#include "../cell_meas_manager/cell_meas_manager_impl.h"
#include "../cell_meas_manager/slice_aware_matching.h"
#include "../du_processor/du_processor_repository.h"
#include "ocudu/cu_cp/up_context.h"
#include "ocudu/ran/nr_cgi.h"
#include <algorithm>
#include <optional>
#include <set>
#include <vector>

using namespace ocudu;
using namespace ocucp;

namespace {

// TS 23.501 clause 5.15.2
std::vector<s_nssai_t> get_ue_active_snssais(cu_cp_ue& ue)
{
  std::vector<s_nssai_t> snssais;
  for (const auto& [psi, session] : ue.get_up_resource_manager().get_pdu_sessions_map()) {
    for (const auto& [drb_id, drb] : session.drbs) {
      if (std::none_of(snssais.begin(), snssais.end(), [&drb](const s_nssai_t& s) { return s == drb.s_nssai; })) {
        snssais.push_back(drb.s_nssai);
      }
    }
  }
  return snssais;
}

} // namespace

mobility_manager::mobility_manager(const mobility_manager_cfg&      cfg_,
                                   mobility_manager_cu_cp_notifier& cu_cp_notifier_,
                                   ngap_repository&                 ngap_db_,
                                   du_processor_repository&         du_db_,
                                   ue_manager&                      ue_mng_,
                                   cell_meas_manager&               cell_meas_mng_) :
  cfg(cfg_),
  cu_cp_notifier(cu_cp_notifier_),
  ngap_db(ngap_db_),
  du_db(du_db_),
  ue_mng(ue_mng_),
  cell_meas_mng(cell_meas_mng_),
  logger(ocudulog::fetch_basic_logger("CU-CP"))
{
}

void mobility_manager::trigger_handover(pci_t source_pci, rnti_t rnti, pci_t target_pci)
{
  ue_index_t ue_index = ue_mng.get_ue_index(source_pci, rnti);
  if (ue_index == ue_index_t::invalid) {
    logger.warning("Could not trigger handover, UE is invalid. rnti={} pci={}", rnti, source_pci);
    return;
  }
  handle_handover(ue_index, gnb_id_t{}, nr_cell_identity{}, target_pci); // TODO: define gNB-ID and NCI
}

void mobility_manager::trigger_conditional_handover(
    pci_t                                                source_pci,
    rnti_t                                               rnti,
    span<const pci_t>                                    target_pcis,
    std::chrono::milliseconds                            timeout,
    std::optional<std::chrono::system_clock::time_point> t1_thres_override)
{
  // Trigger CHO by preparing and then automatically executing.
  handle_conditional_handover(source_pci, rnti, target_pcis, timeout, t1_thres_override);
}

void mobility_manager::handle_conditional_handover(
    pci_t                                                source_pci,
    rnti_t                                               rnti,
    span<const pci_t>                                    target_pcis,
    std::chrono::milliseconds                            timeout,
    std::optional<std::chrono::system_clock::time_point> t1_thres_override)
{
  // Find UE by PCI and RNTI.
  ue_index_t ue_index = ue_mng.get_ue_index(source_pci, rnti);
  if (ue_index == ue_index_t::invalid) {
    logger.warning("Could not prepare CHO, UE is invalid. rnti={} pci={}", rnti, source_pci);
    return;
  }

  // Find the UE context.
  cu_cp_ue* u = ue_mng.find_du_ue(ue_index);
  if (u == nullptr) {
    logger.error("ue={}: Couldn't find UE for CHO preparation", ue_index);
    return;
  }

  // Check UE supports CHO before doing any candidate work.
  if (u->get_rrc_ue() == nullptr || !u->get_rrc_ue()->is_conditional_handover_supported()) {
    logger.warning("ue={}: UE does not support CHO (Rel-16); aborting preparation", ue_index);
    return;
  }

  // Validate target PCIs.
  if (target_pcis.empty()) {
    logger.warning("ue={}: CHO preparation failed. No target PCIs specified", ue_index);
    return;
  }

  std::set<pci_t>    seen_target_pcis;
  std::vector<pci_t> unique_target_pcis;
  unique_target_pcis.reserve(target_pcis.size());
  for (pci_t target_pci : target_pcis) {
    if (seen_target_pcis.insert(target_pci).second) {
      unique_target_pcis.push_back(target_pci);
    }
  }
  if (unique_target_pcis.size() != target_pcis.size()) {
    logger.warning("ue={}: CHO request contains duplicate target PCIs. Duplicates were ignored", ue_index);
  }

  if (unique_target_pcis.size() > 8) {
    logger.warning(
        "ue={}: CHO preparation failed. Too many unique target PCIs ({} > 8)", ue_index, unique_target_pcis.size());
    return;
  }

  // Validate all targets are intra-CU.
  du_index_t source_du = u->get_du_index();
  if (source_du == du_index_t::invalid) {
    logger.warning("ue={}: CHO preparation failed. Source DU index is invalid", ue_index);
    return;
  }

  std::vector<cu_cp_cho_target_candidate> targets;
  for (pci_t target_pci : unique_target_pcis) {
    du_index_t target_du = du_db.find_du(target_pci);
    if (target_du == du_index_t::invalid) {
      logger.warning("ue={}: CHO preparation failed. Target PCI {} not found in any local DU", ue_index, target_pci);
      return;
    }
    // Lookup CGI at target DU.
    std::optional<nr_cell_global_id_t> cgi =
        du_db.get_du_processor(target_du).get_mobility_handler().get_cgi(target_pci);
    if (!cgi.has_value()) {
      logger.warning("ue={}: CHO preparation failed. Could not find CGI for PCI {}", ue_index, target_pci);
      return;
    }
    // TS 23.501 clause 5.15.3; TS 38.413; TS 38.423
    if (not target_cell_supports_ue_slices(ue_index, cgi.value().nci)) {
      logger.info("ue={}: CHO candidate pci={} skipped: target cell does not support all of the UE's active slices",
                  ue_index,
                  target_pci);
      continue;
    }
    targets.push_back({target_pci, cgi.value(), target_du});
  }

  if (targets.empty()) {
    metrics_handler.aggregate_suppressed_handover_slice_mismatch();
    logger.info("ue={}: CHO suppressed: no slice-compatible target candidate", ue_index);
    return;
  }

  // Check if CHO is already pending.
  auto& cho_ctx = u->get_cho_context();
  if (cho_ctx.has_value() && cho_ctx->state != cu_cp_ue_cho_context::state_t::idle) {
    logger.warning(
        "ue={}: CHO preparation failed. CHO already pending (state={})", ue_index, static_cast<int>(cho_ctx->state));
    return;
  }

  // Initialize CHO context.
  if (!cho_ctx.has_value()) {
    cho_ctx.emplace();
  }
  cho_ctx->clear();

  logger.info("ue={}: Starting intra-CU CHO with {} candidate(s)", ue_index, targets.size());

  cu_cp_intra_cu_cho_request cho_request{};
  cho_request.source_ue_index   = ue_index;
  cho_request.source_du_index   = source_du;
  cho_request.targets           = std::move(targets);
  cho_request.timeout           = timeout;
  cho_request.t1_thres_override = t1_thres_override;

  auto cho_trigger = [this, cho_request = std::move(cho_request), cho_response = cu_cp_intra_cu_cho_response{}](
                         coro_context<async_task<void>>& ctx) mutable {
    CORO_BEGIN(ctx);
    CORO_AWAIT_VALUE(cho_response, cu_cp_notifier.on_intra_cu_cho_required(cho_request));
    if (!cho_response.success) {
      logger.warning("ue={}: Intra-CU CHO failed", cho_request.source_ue_index);
    }
    CORO_RETURN();
  };

  u->get_task_sched().schedule_async_task(launch_async(std::move(cho_trigger)));
}

void mobility_manager::handle_neighbor_better_than_spcell(ue_index_t       ue_index,
                                                          gnb_id_t         neighbor_gnb_id,
                                                          nr_cell_identity neighbor_nci,
                                                          pci_t            neighbor_pci)
{
  if (!cfg.trigger_handover_from_measurements) {
    logger.debug("ue={}: Ignoring better neighbor pci={}", ue_index, neighbor_pci);
    return;
  }

  // TS 23.501 clause 5.15.3; TS 38.413; TS 38.423
  if (not target_cell_supports_ue_slices(ue_index, neighbor_nci)) {
    metrics_handler.aggregate_suppressed_handover_slice_mismatch();
    logger.info(
        "ue={}: Handover suppressed: target pci={} (nci={:#x}) does not support all of the UE's active slices",
        ue_index,
        neighbor_pci,
        neighbor_nci);
    return;
  }

  handle_handover(ue_index, neighbor_gnb_id, neighbor_nci, neighbor_pci);
}

bool mobility_manager::target_cell_supports_ue_slices(ue_index_t ue_index, nr_cell_identity target_nci)
{
  cu_cp_ue* u = ue_mng.find_du_ue(ue_index);
  if (u == nullptr) {
    return true;
  }

  const std::vector<s_nssai_t> ue_slices = get_ue_active_snssais(*u);
  if (ue_slices.empty()) {
    logger.debug("ue={}: Slice check skipped: UE has no active DRB/slice", ue_index);
    return true;
  }

  std::optional<cell_meas_config> target_cfg = cell_meas_mng.get_cell_config(target_nci);
  if (not target_cfg.has_value()) {
    return true;
  }

  return cell_supports_all_ue_slices(target_cfg->serving_cell_cfg.supported_slices, ue_slices);
}

void mobility_manager::handle_handover(ue_index_t       ue_index,
                                       gnb_id_t         neighbor_gnb_id,
                                       nr_cell_identity neighbor_nci,
                                       pci_t            neighbor_pci)
{
  // Find the UE context.
  cu_cp_ue* u = ue_mng.find_du_ue(ue_index);
  if (u == nullptr) {
    logger.error("ue={}: Couldn't find UE", ue_index);
    return;
  }
  cu_cp_ue_context& ue_ctxt = u->get_ue_context();
  if (ue_ctxt.reconfiguration_disabled) {
    logger.debug("ue={}: MeasurementReport ignored. Cause: UE cannot be reconfigured", ue_index);
    return;
  }
  if (neighbor_pci == INVALID_PCI) {
    logger.error("ue={}: Ignoring Handover Request. Cause: Invalid target PCI {} received", ue_index, neighbor_pci);
    return;
  }

  // Handover is going ahead.

  // Disable new reconfigurations from now on (except for the Handover Command).
  ue_ctxt.reconfiguration_disabled = true;

  // Try to find target DU. If it is not found, it means that the target cell is not managed by this CU-CP and
  // a NG Handover is required.
  du_index_t target_du = du_db.find_du(neighbor_pci);
  if (target_du == du_index_t::invalid) {
    logger.debug("ue={}: Requesting inter CU handover. No local DU/cell with pci={} found", ue_index, neighbor_pci);
    handle_inter_cu_handover(ue_index, neighbor_gnb_id, neighbor_nci);
    return;
  }

  du_index_t source_du = ue_mng.find_du_ue(ue_index)->get_du_index();

  if (source_du == target_du) {
    logger.info("ue={}: Trigger intra-CU (intra-DU) handover on du={}", ue_index, source_du);
  } else {
    logger.info("ue={}: Trigger intra-CU (inter-DU) handover from source_du={} to target_du={}",
                ue_index,
                source_du,
                target_du);
  }
  handle_intra_cu_handover(ue_index, neighbor_pci, source_du, target_du);
}

void mobility_manager::handle_intra_cu_handover(ue_index_t source_ue_index,
                                                pci_t      neighbor_pci,
                                                du_index_t source_du_index,
                                                du_index_t target_du_index)
{
  // Lookup CGI at target DU.
  std::optional<nr_cell_global_id_t> cgi =
      du_db.get_du_processor(target_du_index).get_mobility_handler().get_cgi(neighbor_pci);
  if (!cgi.has_value()) {
    logger.warning(
        "ue={}: Couldn't retrieve CGI for pci={} at du_index={}", source_ue_index, neighbor_pci, target_du_index);
    return;
  }

  cu_cp_intra_cu_handover_request request = {};
  request.source_ue_index                 = source_ue_index;
  request.target_pci                      = neighbor_pci;
  request.cgi                             = cgi.value();
  request.target_du_index                 = target_du_index;

  cu_cp_ue* u = ue_mng.find_du_ue(source_ue_index);
  if (u == nullptr) {
    logger.error("ue={}: Couldn't find UE", source_ue_index);
    return;
  }

  // Trigger Intra CU handover routine on the DU processor of the source DU.
  auto ho_trigger = [this, request, response = cu_cp_intra_cu_handover_response{}, &source_du_index, &target_du_index](
                        coro_context<async_task<void>>& ctx) mutable {
    CORO_BEGIN(ctx);
    CORO_AWAIT_VALUE(response, cu_cp_notifier.on_intra_cu_handover_required(request, source_du_index, target_du_index));
    CORO_RETURN();
  };
  u->get_task_sched().schedule_async_task(launch_async(std::move(ho_trigger)));
}

void mobility_manager::handle_inter_cu_handover(ue_index_t       source_ue_index,
                                                gnb_id_t         target_gnb_id,
                                                nr_cell_identity target_nci)
{
  cu_cp_ue* u = ue_mng.find_du_ue(source_ue_index);
  if (u == nullptr) {
    logger.error("ue={}: Couldn't find UE", source_ue_index);
    return;
  }

  ngap_handover_preparation_request request = {};
  request.ue_index                          = source_ue_index;
  request.gnb_id                            = target_gnb_id;
  request.nci                               = target_nci;

  // create a map of all PDU sessions and their associated QoS flows
  const std::map<pdu_session_id_t, up_pdu_session_context>& pdu_sessions =
      ue_mng.find_ue(source_ue_index)->get_up_resource_manager().get_pdu_sessions_map();
  for (const auto& pdu_session : pdu_sessions) {
    std::vector<qos_flow_id_t> qos_flows;
    for (const auto& drb : pdu_session.second.drbs) {
      for (const auto& qos_flow : drb.second.qos_flows) {
        qos_flows.push_back(qos_flow.first);
      }
    }
    request.pdu_sessions.insert({pdu_session.first, qos_flows});
  }

  cu_cp_ue_context& ue_ctxt = u->get_ue_context();

  auto* ngap = ngap_db.find_ngap(ue_ctxt.plmn);
  if (ngap == nullptr) {
    logger.error("ue={}: Couldn't find NGAP", source_ue_index);
    return;
  }

  // Send handover preparation request to the NGAP handler.
  auto ho_trigger = [ngap, request, response = ngap_handover_preparation_response{}](
                        coro_context<async_task<void>>& ctx) mutable {
    CORO_BEGIN(ctx);
    CORO_AWAIT_VALUE(response, ngap->get_ngap_control_message_handler().handle_handover_preparation_request(request));
    CORO_RETURN();
  };
  u->get_task_sched().schedule_async_task(launch_async(std::move(ho_trigger)));
}
