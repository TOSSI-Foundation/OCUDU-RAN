// SPDX-FileCopyrightText: Copyright (C) 2021-2026 Software Radio Systems Limited
// SPDX-License-Identifier: BSD-3-Clause-Open-MPI
// Portions of this file may implement 3GPP specifications, which may be subject to additional licensing requirements.

#pragma once

#include "ocudu/ran/pucch/pucch_info.h"
#include "ocudu/ran/ssb/ssb_mapping.h"
#include "ocudu/scheduler/config/ran_cell_config.h"
#include "ocudu/scheduler/config/rlm_helper.h"
#include "ocudu/scheduler/config/sched_cell_config_helpers.h"
#include "ocudu/scheduler/config/serving_cell_config.h"
#include "ocudu/scheduler/config/serving_cell_config_factory.h"
#include "ocudu/scheduler/config/time_domain_resource_helper.h"
#include "ocudu/scheduler/config/ue_bwp_config.h"
#include "ocudu/scheduler/scheduler_configurator.h"

namespace ocudu::config_helpers {

/// Builds a BWP config from a cell config builder parameters.
inline bwp_builder_params make_default_bwp_builder_params(const cell_config_builder_params_extended& params = {})
{
  bwp_builder_params bwp;
  bwp.pdcch_cfg            = make_ue_dedicated_pdcch_config(params);
  bwp.pdsch.max_nof_layers = params.max_nof_layers;
  bwp.pusch.min_k2         = params.min_k2;
  bwp.pucch.min_k1         = params.min_k1;
  bwp.pucch.sr_period      = static_cast<sr_periodicity>(get_nof_slots_per_subframe(params.scs_common) * 40U);
  if (params.csi_rs_enabled) {
    bwp.csi = make_default_csi_builder_params(params).csi_params;
  }
  return bwp;
}

/// Generates default RAN cell configuration used by gNB DU. The default configuration should be valid.
inline ran_cell_config make_default_ran_cell_config(const cell_config_builder_params_extended& params = {})
{
  ran_cell_config cfg;
  cfg.pci           = params.pci;
  cfg.dl_carrier    = make_default_dl_carrier_configuration(params);
  cfg.ul_carrier    = make_default_ul_carrier_configuration(params);
  cfg.dl_cfg_common = make_default_dl_config_common(params);
  cfg.ul_cfg_common = make_default_ul_config_common(params);
  cfg.ssb_cfg       = make_default_ssb_config(params);
  // The CORESET duration of 3 symbols is only permitted if dmrs-typeA-Position is set to 3. Refer TS 38.211, 7.3.2.2.
  cfg.dmrs_typeA_pos       = cfg.dl_cfg_common.init_dl_bwp.pdcch_common.coreset0.value().duration() >= 3U
                                 ? dmrs_typeA_position::pos3
                                 : dmrs_typeA_position::pos2;
  cfg.tdd_ul_dl_cfg_common = params.tdd_ul_dl_cfg_common;
  cfg.init_bwp_builder     = make_default_bwp_builder_params(params);
  return cfg;
}

/// Builds CSI meas config builder parameters from DU cell configuration.
inline csi_helper::csi_meas_config_builder_params make_csi_meas_config_builder_params(const ran_cell_config& cell_cfg)
{
  ocudu_assert(cell_cfg.init_bwp_builder.csi.has_value(), "CSI parameters are required to build CSI resources");

  csi_helper::csi_meas_config_builder_params csi_params;
  csi_params.pci            = cell_cfg.pci;
  csi_params.nof_rbs        = cell_cfg.ul_cfg_common.init_ul_bwp.generic_params.crbs.length();
  csi_params.nof_ports      = cell_cfg.dl_carrier.nof_ant;
  csi_params.max_nof_layers = cell_cfg.init_bwp_builder.pdsch.max_nof_layers.value_or(csi_params.nof_ports);
  csi_params.mcs_table      = cell_cfg.init_bwp_builder.pdsch.mcs_table;
  csi_params.csi_params     = cell_cfg.init_bwp_builder.csi.value();
  return csi_params;
}

/// Builds a CSI-MeasConfig from DU cell configuration.
inline std::optional<csi_meas_config> make_csi_meas_config(const ran_cell_config& cell_cfg)
{
  if (!cell_cfg.init_bwp_builder.csi.has_value()) {
    return std::nullopt;
  }
  const csi_helper::csi_meas_config_builder_params csi_params = make_csi_meas_config_builder_params(cell_cfg);
  csi_meas_config                                  csi_cfg    = csi_helper::make_csi_meas_config(
      csi_params, cell_cfg.ul_cfg_common.init_ul_bwp.pusch_cfg_common->pusch_td_alloc_list);
  return csi_cfg;
}

/// Builds a PUCCH configuration from DU cell configuration and PUCCH builder parameters.
inline pucch_config make_pucch_config(const ran_cell_config& cell_cfg)
{
  pucch_config                         pucch_cfg{};
  const pucch_builder_params&          pucch_params   = cell_cfg.init_bwp_builder.pucch;
  const pucch_resource_builder_params& builder_params = pucch_params.resources;
  const unsigned                       bwp_size       = cell_cfg.ul_cfg_common.init_ul_bwp.generic_params.crbs.length();
  const auto                           cell_pucch_res_list = build_pucch_resource_list(builder_params, bwp_size);

  // >>> Resource Set ID 0.
  {
    auto& res_set_0            = pucch_cfg.pucch_res_set.emplace_back();
    res_set_0.pucch_res_set_id = pucch_res_set_idx::set_0;
    for (unsigned r_pucch = 0; r_pucch != builder_params.res_set_0_size; ++r_pucch) {
      auto& res            = pucch_cfg.pucch_res_list.emplace_back(cell_pucch_res_list[r_pucch]);
      res.res_id.ue_res_id = pucch_cfg.pucch_res_list.size() - 1;
      res_set_0.pucch_res_id_list.push_back(res.res_id);
    }
  }

  // >>> SR Resource.
  {
    const sr_periodicity sr_period = pucch_params.sr_period;
    const unsigned       sr_offset = cell_cfg.tdd_ul_dl_cfg_common.has_value()
                                         ? find_next_tdd_full_ul_slot(*cell_cfg.tdd_ul_dl_cfg_common).value()
                                         : 0;
    auto& sr_res = pucch_cfg.pucch_res_list.emplace_back(cell_pucch_res_list[builder_params.res_set_0_size.value()]);
    sr_res.res_id.ue_res_id = pucch_cfg.pucch_res_list.size() - 1;
    pucch_cfg.sr_res_list.push_back(scheduling_request_resource_config{.sr_res_id    = 1,
                                                                       .sr_id        = uint_to_sched_req_id(0),
                                                                       .period       = sr_period,
                                                                       .offset       = sr_offset,
                                                                       .pucch_res_id = sr_res.res_id});
  }

  // >>> Resource Set ID 1.
  {
    auto& res_set_1            = pucch_cfg.pucch_res_set.emplace_back();
    res_set_1.pucch_res_set_id = pucch_res_set_idx::set_1;
    const unsigned res_set_0_cell_res_offset =
        builder_params.res_set_0_size.value() * builder_params.nof_cell_res_set_configs +
        builder_params.nof_cell_sr_resources;
    for (unsigned r_pucch = 0; r_pucch != builder_params.res_set_1_size; ++r_pucch) {
      auto& res = pucch_cfg.pucch_res_list.emplace_back(cell_pucch_res_list[res_set_0_cell_res_offset + r_pucch]);
      res.res_id.ue_res_id = pucch_cfg.pucch_res_list.size() - 1;
      res_set_1.pucch_res_id_list.push_back(res.res_id);
    }
  }

  // >>> CSI resource.
  if (builder_params.nof_cell_csi_resources > 0) {
    const unsigned csi_cell_res_offset =
        builder_params.res_set_0_size.value() * builder_params.nof_cell_res_set_configs +
        builder_params.nof_cell_sr_resources +
        builder_params.res_set_1_size.value() * builder_params.nof_cell_res_set_configs;
    auto& res            = pucch_cfg.pucch_res_list.emplace_back(cell_pucch_res_list[csi_cell_res_offset]);
    res.res_id.ue_res_id = pucch_cfg.pucch_res_list.size() - 1;
  }

  // Increase code rate in case of more than 4 layers.
  if (std::holds_alternative<pucch_f1_params>(builder_params.f0_or_f1_params)) {
    pucch_cfg.format_1_common_param.emplace();
  }
  if (const auto* f2_params = std::get_if<pucch_f2_params>(&builder_params.f2_or_f3_or_f4_params)) {
    pucch_cfg.format_2_common_param.emplace(
        pucch_common_all_formats{.max_c_rate = f2_params->max_code_rate, .simultaneous_harq_ack_csi = true});
  } else if (const auto* f3_params = std::get_if<pucch_f3_params>(&builder_params.f2_or_f3_or_f4_params)) {
    pucch_cfg.format_3_common_param.emplace(
        pucch_common_all_formats{.max_c_rate = f3_params->max_code_rate, .simultaneous_harq_ack_csi = true});
  } else if (const auto* f4_params = std::get_if<pucch_f4_params>(&builder_params.f2_or_f3_or_f4_params)) {
    pucch_cfg.format_4_common_param.emplace(
        pucch_common_all_formats{.max_c_rate = f4_params->max_code_rate, .simultaneous_harq_ack_csi = true});
  }
  pucch_cfg.dl_data_to_ul_ack =
      time_domain_resource_helper::generate_k1_candidates(cell_cfg.tdd_ul_dl_cfg_common, pucch_params.min_k1);

  // Compute the max UCI payload per format.
  // As per TS 38.231, Section 9.2.1, with PUCCH Format 1, we can have up to 2 HARQ-ACK bits (SR doesn't count as part
  // of the payload).
  {
    static constexpr unsigned pucch_f1_max_harq_payload                        = 2U;
    pucch_cfg.format_max_payload[pucch_format_to_uint(pucch_format::FORMAT_1)] = pucch_f1_max_harq_payload;

    if (std::holds_alternative<pucch_f2_params>(builder_params.f2_or_f3_or_f4_params)) {
      auto* res_f2_it = std::find_if(pucch_cfg.pucch_res_list.begin(),
                                     pucch_cfg.pucch_res_list.end(),
                                     [](const pucch_resource& res) { return res.format == pucch_format::FORMAT_2; });
      ocudu_assert(res_f2_it != pucch_cfg.pucch_res_list.end(), "No PUCCH F2 resource found in the cell configuration");
      const auto& res_f2_params = std::get<pucch_format_2_3_cfg>(res_f2_it->format_params);
      pucch_cfg.format_max_payload[pucch_format_to_uint(pucch_format::FORMAT_2)] =
          get_pucch_format2_max_payload(res_f2_params.nof_prbs,
                                        res_f2_it->nof_symbols,
                                        to_max_code_rate_float(pucch_cfg.format_2_common_param.value().max_c_rate));
      pucch_cfg.set_1_format = pucch_format::FORMAT_2;
    } else if (std::holds_alternative<pucch_f3_params>(builder_params.f2_or_f3_or_f4_params)) {
      auto* res_f3_it = std::find_if(pucch_cfg.pucch_res_list.begin(),
                                     pucch_cfg.pucch_res_list.end(),
                                     [](const pucch_resource& res) { return res.format == pucch_format::FORMAT_3; });
      ocudu_assert(res_f3_it != pucch_cfg.pucch_res_list.end(), "No PUCCH F3 resource found in the cell configuration");
      const auto& f3_common_params = pucch_cfg.format_3_common_param.value();
      pucch_cfg.format_max_payload[pucch_format_to_uint(pucch_format::FORMAT_3)] =
          get_pucch_format3_max_payload(std::get<pucch_format_2_3_cfg>(res_f3_it->format_params).nof_prbs,
                                        res_f3_it->nof_symbols,
                                        to_max_code_rate_float(pucch_cfg.format_3_common_param.value().max_c_rate),
                                        res_f3_it->second_hop_prb.has_value(),
                                        f3_common_params.additional_dmrs,
                                        f3_common_params.pi_2_bpsk);
      pucch_cfg.set_1_format = pucch_format::FORMAT_3;
    } else {
      auto* res_f4_it = std::find_if(pucch_cfg.pucch_res_list.begin(),
                                     pucch_cfg.pucch_res_list.end(),
                                     [](const pucch_resource& res) { return res.format == pucch_format::FORMAT_4; });
      ocudu_assert(res_f4_it != pucch_cfg.pucch_res_list.end(), "No PUCCH F4 resource found in the cell configuration");
      const auto& f4_common_params = pucch_cfg.format_4_common_param.value();
      pucch_cfg.format_max_payload[pucch_format_to_uint(pucch_format::FORMAT_4)] =
          get_pucch_format4_max_payload(res_f4_it->nof_symbols,
                                        to_max_code_rate_float(pucch_cfg.format_4_common_param.value().max_c_rate),
                                        res_f4_it->second_hop_prb.has_value(),
                                        f4_common_params.additional_dmrs,
                                        f4_common_params.pi_2_bpsk,
                                        std::get<pucch_format_4_cfg>(res_f4_it->format_params).occ_length);
      pucch_cfg.set_1_format = pucch_format::FORMAT_4;
    }
  }

  // Add the PUCCH power configuration.
  auto& pucch_pw_ctrl          = pucch_cfg.pucch_pw_control.emplace();
  pucch_pw_ctrl.delta_pucch_f0 = 0;
  pucch_pw_ctrl.delta_pucch_f1 = 0;
  pucch_pw_ctrl.delta_pucch_f2 = 0;
  pucch_pw_ctrl.delta_pucch_f3 = 0;
  pucch_pw_ctrl.delta_pucch_f4 = 0;
  auto& pucch_pw_set           = pucch_pw_ctrl.p0_set.emplace_back();
  pucch_pw_set.id              = 0U;
  pucch_pw_set.value           = 0;

  return pucch_cfg;
}

/// Builds an SRS configuration from SRS builder parameters.
inline srs_config make_srs_config(const srs_builder_params& user_params, pci_t pci)
{
  srs_config cfg{};

  cfg.srs_res_list.emplace_back();
  srs_config::srs_resource& res    = cfg.srs_res_list.back();
  res.id.cell_res_id               = 0;
  res.id.ue_res_id                 = static_cast<srs_config::srs_res_id>(0U);
  res.nof_ports                    = srs_config::srs_resource::nof_srs_ports::port1;
  res.tx_comb.size                 = user_params.tx_comb;
  res.tx_comb.tx_comb_offset       = 0;
  res.tx_comb.tx_comb_cyclic_shift = 0;
  const auto nof_symb              = static_cast<uint8_t>(user_params.nof_symbols);
  res.res_mapping.start_pos        = nof_symb - 1U;
  res.res_mapping.nof_symb         = user_params.nof_symbols;
  res.res_mapping.rept_factor      = srs_nof_symbols::n1;
  res.freq_domain_pos              = 0;
  res.freq_domain_shift            = user_params.c_srs.has_value() ? user_params.freq_domain_shift.value() : 0;
  res.freq_hop.c_srs               = user_params.c_srs.value_or(0);
  // We assume that the frequency hopping is disabled and that the SRS occupies all possible RBs within the BWP. Refer
  // to Section 6.4.1.4.3, TS 38.211.
  res.freq_hop.b_srs = 0;
  res.freq_hop.b_hop = 0;
  res.grp_or_seq_hop = srs_group_or_sequence_hopping::neither;
  res.sequence_id    = pci;

  cfg.srs_res_set_list.emplace_back();
  srs_config::srs_resource_set& res_set = cfg.srs_res_set_list.back();
  // Set the SRS resource set ID to 0, as there is only 1 SRS resource set per UE.
  res_set.id = static_cast<srs_config::srs_res_set_id>(0U);
  res_set.srs_res_id_list.emplace_back(static_cast<srs_config::srs_res_id>(0U));
  res_set.srs_res_set_usage = srs_usage::codebook;
  res_set.p0                = user_params.p0;
  res_set.pathloss_ref_rs   = static_cast<ssb_id_t>(0);

  if (user_params.srs_type_enabled == srs_type::periodic) {
    res.res_type = srs_resource_type::periodic;
    // Set offset to 0. The offset will be updated later on, when the UE is allocated the SRS resources.
    res.periodicity_and_offset.emplace(
        srs_config::srs_periodicity_and_offset{.period = user_params.srs_period_prohib_time, .offset = 0});
    res_set.res_type.emplace<srs_config::srs_resource_set::periodic_resource_type>(
        srs_config::srs_resource_set::periodic_resource_type{});
  } else {
    // In case of aperiodic or disabled, we create an aperiodic SRS resource.
    res.res_type = srs_resource_type::aperiodic;
    res_set.res_type =
        srs_config::srs_resource_set::aperiodic_resource_type{.aperiodic_srs_res_trigger = 1, .slot_offset = 7};
  }
  return cfg;
}

/// Builds a Radio Link Monitoring configuration from DU cell configuration.
inline std::optional<radio_link_monitoring_config> make_rlm_config(const ran_cell_config& cell_cfg)
{
  const rlm_resource_type rlm_type = cell_cfg.init_bwp_builder.rlm.type;
  if (rlm_type == rlm_resource_type::default_type) {
    return std::nullopt;
  }

  const uint8_t l_max = ssb_get_L_max(cell_cfg.ssb_cfg.scs, cell_cfg.dl_carrier.arfcn_f_ref, cell_cfg.dl_carrier.band);

  rlm_helper::rlm_builder_params rlm_params;
  if (rlm_type == rlm_resource_type::ssb || rlm_type == rlm_resource_type::ssb_and_csi_rs) {
    rlm_params =
        rlm_helper::rlm_builder_params(rlm_type, l_max, cell_cfg.ssb_cfg.ssb_bitmap, cell_cfg.ssb_cfg.beam_ids);
  } else {
    rlm_params = rlm_helper::rlm_builder_params(rlm_type, l_max);
  }

  span<const nzp_csi_rs_resource> csi_rs_resources;
  const auto                      csi_meas_cfg = make_csi_meas_config(cell_cfg);
  if (csi_meas_cfg.has_value()) {
    csi_rs_resources = csi_meas_cfg->nzp_csi_rs_res_list;
  }

  radio_link_monitoring_config rlm_cfg = rlm_helper::make_radio_link_monitoring_config(rlm_params, csi_rs_resources);
  if (rlm_cfg.rlm_resources.empty()) {
    return std::nullopt;
  }
  return rlm_cfg;
}

/// Builds a PDSCH configuration from DU cell configuration.
inline std::optional<pdsch_config> make_pdsch_config(const ran_cell_config& cell_cfg)
{
  pdsch_config pdsch_cfg;

  pdsch_cfg.pdsch_mapping_type_a_dmrs.emplace();
  pdsch_cfg.pdsch_mapping_type_a_dmrs->additional_positions = cell_cfg.init_bwp_builder.pdsch.additional_positions;

  // TS 38.214 Section 5.1.6.2, TS 38.331 PDSCH-Config
  if (cell_cfg.init_bwp_builder.pdsch.enable_pdsch_mapping_type_b) {
    pdsch_cfg.pdsch_mapping_type_b_dmrs.emplace();
    pdsch_cfg.pdsch_mapping_type_b_dmrs->additional_positions = cell_cfg.init_bwp_builder.pdsch.additional_positions;
  }

  pdsch_cfg.tci_states.push_back(tci_state{
      .state_id  = static_cast<tci_state_id_t>(0),
      .qcl_type1 = {.ref_sig  = {.type = qcl_info::reference_signal::reference_signal_type::ssb,
                                 .ssb  = static_cast<ssb_id_t>(0)},
                    .qcl_type = qcl_info::qcl_type::type_d},
  });

  pdsch_cfg.res_alloc = pdsch_config::resource_allocation::resource_allocation_type_1;
  pdsch_cfg.rbg_sz    = rbg_size::config1;

  pdsch_cfg.vrb_to_prb_interleaving = cell_cfg.init_bwp_builder.pdsch.interleaving_bundle_size;
  pdsch_cfg.mcs_table               = cell_cfg.init_bwp_builder.pdsch.mcs_table;

  pdsch_cfg.harq_process_num_size_dci_1_1 = pdsch_config::harq_process_num_dci_1_1_size::n4;
  pdsch_cfg.harq_process_num_size_dci_1_2 = pdsch_config::harq_process_num_dci_1_2_size::n4;

  // According to TS 38.214 Section 5.1.2.3, prb-BundlingType size must match the VRB-to-PRB mapping type.
  switch (pdsch_cfg.vrb_to_prb_interleaving) {
    case vrb_to_prb::mapping_type::non_interleaved:
      // > If $P'_{BWP,i}$ is determined as "wideband", the UE is not expected to be scheduled with non-contiguous
      // > PRBs and the UE may assume that the same precoding is applied to the allocated resource.
      pdsch_cfg.prb_bndlg.bundling.emplace<prb_bundling::static_bundling>(
          prb_bundling::static_bundling({.sz = prb_bundling::static_bundling::bundling_size::wideband}));
      break;
    case vrb_to_prb::mapping_type::interleaved_n2:
      // > When a UE is configured with nominal RBG size = 2 for bandwidth part i according to clause 5.1.2.2.1, or
      // > when a UE is configured with interleaving unit of 2 for VRB to PRB mapping provided by the higher layer
      // > parameter vrb-ToPRB-Interleaver given by PDSCH-Config for bandwidth part i, the UE is not expected to be
      // > configured with $P'_{BWP,i} = 4$.
      pdsch_cfg.prb_bndlg.bundling.emplace<prb_bundling::static_bundling>(
          prb_bundling::static_bundling({.sz = std::nullopt}));
      break;
    case vrb_to_prb::mapping_type::interleaved_n4:
      pdsch_cfg.prb_bndlg.bundling.emplace<prb_bundling::static_bundling>(
          prb_bundling::static_bundling({.sz = prb_bundling::static_bundling::bundling_size::n4}));
      break;
  }

  if (cell_cfg.init_bwp_builder.csi.has_value()) {
    const csi_helper::csi_meas_config_builder_params csi_params = make_csi_meas_config_builder_params(cell_cfg);
    pdsch_cfg.zp_csi_rs_res_list = csi_helper::make_periodic_zp_csi_rs_resource_list(csi_params);
    pdsch_cfg.p_zp_csi_rs_res    = csi_helper::make_periodic_zp_csi_rs_resource_set(csi_params);
  }

  return pdsch_cfg;
}

/// Builds a PUSCH configuration from PUSCH builder parameters.
inline pusch_config make_pusch_config(const pusch_builder_params& pusch_params)
{
  pusch_config pusch_cfg = make_default_pusch_config();

  pusch_cfg.mcs_table = pusch_params.mcs_table;
  if (!pusch_cfg.pusch_mapping_type_a_dmrs.has_value()) {
    pusch_cfg.pusch_mapping_type_a_dmrs.emplace();
  }
  pusch_cfg.pusch_mapping_type_a_dmrs->additional_positions = pusch_params.additional_positions;

  if (pusch_params.transform_precoding_enabled) {
    pusch_cfg.trans_precoder = pusch_config::transform_precoder::enabled;
    pusch_cfg.pusch_mapping_type_a_dmrs->trans_precoder_enabled.emplace(
        dmrs_uplink_config::transform_precoder_enabled{std::nullopt, false, false});
  }
  if (pusch_params.uci_beta_offsets.has_value()) {
    if (!pusch_cfg.uci_cfg.has_value()) {
      pusch_cfg.uci_cfg.emplace();
    }
    pusch_cfg.uci_cfg->beta_offsets_cfg = uci_on_pusch::beta_offsets_semi_static{pusch_params.uci_beta_offsets.value()};
  }
  if (pusch_params.p0_pusch_alpha.has_value()) {
    if (!pusch_cfg.pusch_pwr_ctrl.has_value()) {
      pusch_cfg.pusch_pwr_ctrl.emplace(pusch_config::pusch_power_control{});
    }
    if (pusch_cfg.pusch_pwr_ctrl->p0_alphasets.empty()) {
      pusch_cfg.pusch_pwr_ctrl->p0_alphasets.emplace_back();
    }
    pusch_cfg.pusch_pwr_ctrl->p0_alphasets.front().p0_pusch_alpha = pusch_params.p0_pusch_alpha.value();
  }

  return pusch_cfg;
}

/// Builds a PDSCH serving cell configuration from PDSCH builder parameters.
inline pdsch_serving_cell_config make_pdsch_serv_cell_config(const pdsch_builder_params& pdsch_params)
{
  pdsch_serving_cell_config cfg = make_default_pdsch_serving_cell_config();
  cfg.nof_harq_proc = static_cast<pdsch_serving_cell_config::nof_harq_proc_for_pdsch>(pdsch_params.nof_harq_procs);
  cfg.dl_harq_feedback_disabled = pdsch_params.dl_harq_feedback_disabled;
  return cfg;
}

/// Builds a PUSCH serving cell configuration from PUSCH builder parameters.
inline pusch_serving_cell_config make_pusch_serv_cell_config(const pusch_builder_params& pusch_params)
{
  pusch_serving_cell_config cfg{};
  if (pusch_params.cbg_tx) {
    cfg.cbg_tx.emplace();
    cfg.cbg_tx->max_cgb_per_tb = static_cast<
        pusch_serving_cell_config::pusch_code_block_group_transmission::max_code_block_groups_per_transport_block>(
        *pusch_params.cbg_tx);
  }
  cfg.x_ov_head     = pusch_params.x_ov_head;
  cfg.nof_harq_proc = static_cast<pusch_serving_cell_config::nof_harq_proc_for_pusch>(pusch_params.nof_harq_procs);
  cfg.ul_harq_mode  = pusch_params.ul_harq_mode;
  return cfg;
}

/// Builds a full UE serving cell configuration from DU cell configuration and a cell index.
inline serving_cell_config make_ue_serving_cell_config(const ran_cell_config& cell_cfg, du_cell_index_t cell_index)
{
  serving_cell_config cfg{};
  cfg.cell_index            = cell_index;
  cfg.init_dl_bwp.pdcch_cfg = cell_cfg.init_bwp_builder.pdcch_cfg;
  cfg.pdsch_serv_cell_cfg   = make_pdsch_serv_cell_config(cell_cfg.init_bwp_builder.pdsch);
  if (!cfg.ul_config.has_value()) {
    cfg.ul_config.emplace();
  }
  cfg.ul_config->init_ul_bwp.pucch_cfg = make_pucch_config(cell_cfg);
  cfg.ul_config->init_ul_bwp.srs_cfg   = make_srs_config(cell_cfg.init_bwp_builder.srs_cfg, cell_cfg.pci);
  cfg.ul_config->init_ul_bwp.pusch_cfg = make_pusch_config(cell_cfg.init_bwp_builder.pusch);
  cfg.ul_config->pusch_serv_cell_cfg   = make_pusch_serv_cell_config(cell_cfg.init_bwp_builder.pusch);
  cfg.csi_meas_cfg                     = make_csi_meas_config(cell_cfg);
  cfg.init_dl_bwp.pdsch_cfg            = make_pdsch_config(cell_cfg);
  cfg.init_dl_bwp.rlm_cfg              = make_rlm_config(cell_cfg);

  // Align CSI PUCCH resource ids with the PUCCH config built from builder params.
  // Note: This is temporary, while a make_csi_meas_config is not present.
  if (cfg.csi_meas_cfg.has_value() && cfg.ul_config->init_ul_bwp.pucch_cfg.has_value()) {
    auto&       csi_meas_cfg = cfg.csi_meas_cfg.value();
    const auto& pucch_cfg    = cfg.ul_config->init_ul_bwp.pucch_cfg.value();
    if (!pucch_cfg.pucch_res_list.empty()) {
      const pucch_res_id_t csi_pucch_res_id = pucch_cfg.pucch_res_list.back().res_id;
      for (auto& report_cfg : csi_meas_cfg.csi_report_cfg_list) {
        if (std::holds_alternative<csi_report_config::periodic_or_semi_persistent_report_on_pucch>(
                report_cfg.report_cfg_type)) {
          auto& pucch_report =
              std::get<csi_report_config::periodic_or_semi_persistent_report_on_pucch>(report_cfg.report_cfg_type);
          for (auto& pucch_res : pucch_report.pucch_csi_res_list) {
            pucch_res.pucch_res_id = csi_pucch_res_id;
          }
        }
      }
    }
  }
  return cfg;
}

inline ue_bwp_config make_ue_bwp_config(const ran_cell_config& cell_cfg)
{
  /// Note: this function is a temporary solution to make sure the BWP configuration mirrors the default \c
  /// serving_cell_config generated by these helpers. After the refactor is done, we should aim at removing this
  /// function and setting all parameters directly with RRM/capability manager.
  ue_bwp_config cfg{};

  cfg.ul.pucch.res_set_cfg_id = pucch_resource_set_config_id(0);
  cfg.ul.pucch.sr_res_id      = pucch_sr_resource_id(0);
  cfg.ul.pucch.sr_offset      = cell_cfg.tdd_ul_dl_cfg_common.has_value()
                                    ? find_next_tdd_full_ul_slot(*cell_cfg.tdd_ul_dl_cfg_common).value()
                                    : 0;

  cfg.ul.pusch.tx_cfg = tx_scheme_codebook{.max_rank = 1, .codebook_subset = tx_scheme_codebook_subset::non_coherent};

  cfg.ul.srs.nof_ports = srs_config::srs_resource::nof_srs_ports::port1;

  if (cell_cfg.init_bwp_builder.csi.has_value() and not cell_cfg.init_bwp_builder.csi->enable_aperiodic_report) {
    const unsigned csi_offset = cell_cfg.init_bwp_builder.csi->csi_report_slot_offset.value();
    cfg.ul.periodic_csi_report.emplace(
        ue_periodic_csi_config{.pucch_res_id = pucch_csi_resource_id(0), .offset = csi_offset});
  }

  return cfg;
}

inline ue_cell_config make_ue_cell_config(const ran_cell_config& cell_cfg, du_cell_index_t cell_index)
{
  ue_cell_config cfg{};

  cfg.serv_cell_cfg = make_ue_serving_cell_config(cell_cfg, cell_index);
  cfg.bwps.emplace_back(make_ue_bwp_config(cell_cfg));

  return cfg;
}

} // namespace ocudu::config_helpers
