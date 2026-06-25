// SPDX-FileCopyrightText: Copyright (C) 2021-2026 Software Radio Systems Limited
// SPDX-License-Identifier: BSD-3-Clause-Open-MPI
// Portions of this file may implement 3GPP specifications, which may be subject to additional licensing requirements.

#pragma once

#include "../ngap_repository.h"
#include "../ue_manager/ue_manager_impl.h"
#include "metrics/mobility_manager_metrics_aggregator.h"
#include "ocudu/cu_cp/cu_cp_command_handler.h"
#include "ocudu/cu_cp/cu_cp_f1c_handler.h"
#include "ocudu/cu_cp/cu_cp_types.h"
#include "ocudu/cu_cp/mobility_manager_config.h"

namespace ocudu {
namespace ocucp {

class du_processor_repository;
class cell_meas_manager;

/// Handler for measurement related events.
class mobility_manager_measurement_handler
{
public:
  virtual ~mobility_manager_measurement_handler() = default;

  /// \brief Handle event where neighbor became better than serving cell.
  virtual void handle_neighbor_better_than_spcell(ue_index_t       ue_index,
                                                  gnb_id_t         neighbor_gnb_id,
                                                  nr_cell_identity neighbor_nci,
                                                  pci_t            neighbor_pci) = 0;
};

/// Interface used to capture the mobility management metrics to the CU-CP.
class mobility_manager_metrics_handler
{
public:
  virtual ~mobility_manager_metrics_handler() = default;

  /// \brief Handle new metrics request for the mobility manager of the CU-CP.
  virtual mobility_management_metrics handle_mobility_metrics_report_request() const = 0;
};

/// Basic mobility manager implementation.
class mobility_manager final : public mobility_manager_measurement_handler,
                               public cu_cp_mobility_command_handler,
                               public mobility_manager_metrics_handler
{
public:
  mobility_manager(const mobility_manager_cfg&      cfg,
                   mobility_manager_cu_cp_notifier& cu_cp_notifier_,
                   ngap_repository&                 ngap_db_,
                   du_processor_repository&         du_db_,
                   ue_manager&                      ue_mng_,
                   cell_meas_manager&               cell_meas_mng_);

  void trigger_handover(pci_t source_pci, rnti_t rnti, pci_t target_pci) override;

  void trigger_conditional_handover(
      pci_t                                                source_pci,
      rnti_t                                               rnti,
      span<const pci_t>                                    target_pcis,
      std::chrono::milliseconds                            timeout,
      std::optional<std::chrono::system_clock::time_point> t1_thres_override = std::nullopt) override;

  void handle_neighbor_better_than_spcell(ue_index_t       ue_index,
                                          gnb_id_t         neighbor_gnb_id,
                                          nr_cell_identity neighbor_nci,
                                          pci_t            neighbor_pci) override;

  mobility_manager_metrics_aggregator& get_metrics_handler() { return metrics_handler; }

  mobility_management_metrics handle_mobility_metrics_report_request() const override
  {
    return metrics_handler.request_metrics_report();
  }

private:
  void
  handle_handover(ue_index_t ue_index, gnb_id_t neighbor_gnb_id, nr_cell_identity neighbor_nci, pci_t neighbor_pci);
  void handle_inter_cu_handover(ue_index_t source_ue_index, gnb_id_t target_gnb_id, nr_cell_identity target_nci);
  void handle_intra_cu_handover(ue_index_t source_ue_index,
                                pci_t      neighbor_pci,
                                du_index_t source_du_index,
                                du_index_t target_du_index);

  void handle_conditional_handover(pci_t                                                source_pci,
                                   rnti_t                                               rnti,
                                   span<const pci_t>                                    target_pcis,
                                   std::chrono::milliseconds                            timeout,
                                   std::optional<std::chrono::system_clock::time_point> t1_thres_override);

  /// TS 23.501 clause 5.15.3; TS 38.413; TS 38.423
  bool target_cell_supports_ue_slices(ue_index_t ue_index, nr_cell_identity target_nci);

  mobility_manager_cfg             cfg;
  mobility_manager_cu_cp_notifier& cu_cp_notifier;
  ngap_repository&                 ngap_db;
  du_processor_repository&         du_db;
  ue_manager&                      ue_mng;
  cell_meas_manager&               cell_meas_mng;

  mobility_manager_metrics_aggregator metrics_handler;

  ocudulog::basic_logger& logger;
};

} // namespace ocucp
} // namespace ocudu
