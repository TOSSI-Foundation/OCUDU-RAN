// SPDX-FileCopyrightText: Copyright (C) 2021-2026 Software Radio Systems Limited
// SPDX-License-Identifier: BSD-3-Clause-Open-MPI
// Portions of this file may implement 3GPP specifications, which may be subject to additional licensing requirements.

#include "ocudu/scheduler/config/serving_cell_config_factory.h"
#include "ocudu/ran/duplex_mode.h"
#include "ocudu/ran/pdcch/pdcch_candidates.h"
#include "ocudu/ran/pdcch/pdcch_type0_css_coreset_config.h"
#include "ocudu/ran/pdcch/search_space.h"
#include "ocudu/ran/prach/prach_configuration.h"
#include "ocudu/ran/prach/prach_helper.h"
#include "ocudu/ran/pucch/pucch_configuration.h"
#include "ocudu/ran/pucch/pucch_info.h"
#include "ocudu/ran/ssb/ssb_mapping.h"
#include "ocudu/scheduler/config/csi_helper.h"
#include "ocudu/scheduler/config/pucch_resource_builder_params.h"
#include "ocudu/scheduler/config/sched_cell_config_helpers.h"
#include "ocudu/scheduler/config/time_domain_resource_helper.h"
#include "ocudu/scheduler/config/ue_bwp_config.h"
#include <algorithm>
#include <vector>

using namespace ocudu;
using namespace ocudu::config_helpers;

cell_config_builder_params_extended::cell_config_builder_params_extended(const cell_config_builder_params& source) :
  cell_config_builder_params(source)
{
  auto_derive_params();

  cell_nof_crbs =
      band_helper::get_n_rbs_from_bw(dl_carrier.carrier_bw, scs_common, band_helper::get_freq_range(dl_carrier.band));

  // If TDD band and TDD pattern has not been specified, generate a default one.
  if (not tdd_ul_dl_cfg_common.has_value() and band_helper::get_duplex_mode(dl_carrier.band) == duplex_mode::TDD) {
    tdd_ul_dl_cfg_common.emplace();
    tdd_ul_dl_cfg_common->ref_scs                            = scs_common;
    tdd_ul_dl_cfg_common->pattern1.dl_ul_tx_period_nof_slots = 10;
    tdd_ul_dl_cfg_common->pattern1.nof_dl_slots              = 6;
    tdd_ul_dl_cfg_common->pattern1.nof_dl_symbols            = 8;
    tdd_ul_dl_cfg_common->pattern1.nof_ul_slots              = 3;
    tdd_ul_dl_cfg_common->pattern1.nof_ul_symbols            = 0;
  } else if (tdd_ul_dl_cfg_common.has_value() and band_helper::get_duplex_mode(dl_carrier.band) != duplex_mode::TDD) {
    report_error("TDD pattern has been set for non-TDD band\n");
  }

  // Compute and store final SSB position based on (selected) values.
  ssb_arfcn = band_helper::get_ssb_arfcn(dl_carrier.arfcn_f_ref,
                                         dl_carrier.band,
                                         cell_nof_crbs,
                                         scs_common,
                                         *scs_ssb,
                                         offset_to_point_a.value(),
                                         k_ssb.value());
  ocudu_assert(ssb_arfcn.has_value(), "Unable to derive SSB location correctly");
}

static carrier_configuration make_default_carrier_configuration(const cell_config_builder_params_extended& params,
                                                                bool                                       is_dl)
{
  carrier_configuration cfg = params.dl_carrier;
  if (not is_dl) {
    cfg.arfcn_f_ref = band_helper::get_ul_arfcn_from_dl_arfcn(params.dl_carrier.arfcn_f_ref, cfg.band);
  }
  const min_channel_bandwidth min_channel_bw = band_helper::get_min_channel_bw(cfg.band, params.scs_common);
  ocudu_assert(bs_channel_bandwidth_to_MHz(cfg.carrier_bw) >= min_channel_bandwidth_to_MHz(min_channel_bw),
               "Carrier BW {}Mhz must be greater than or equal to minimum channel BW {}Mhz",
               bs_channel_bandwidth_to_MHz(cfg.carrier_bw),
               min_channel_bandwidth_to_MHz(min_channel_bw));
  return cfg;
}

carrier_configuration
ocudu::config_helpers::make_default_dl_carrier_configuration(const cell_config_builder_params_extended& params)
{
  return make_default_carrier_configuration(params, true);
}

carrier_configuration
ocudu::config_helpers::make_default_ul_carrier_configuration(const cell_config_builder_params_extended& params)
{
  return make_default_carrier_configuration(params, false);
}

coreset_configuration
ocudu::config_helpers::make_default_coreset_config(const cell_config_builder_params_extended& params, coreset_id cs_id)
{
  // PRBs spanning the maximnum number of CRBs possible.
  freq_resource_bitmap freq_resources(pdcch_constants::MAX_NOF_FREQ_RESOURCES);
  const unsigned       coreset_nof_resources = params.cell_nof_crbs / pdcch_constants::NOF_RB_PER_FREQ_RESOURCE;
  freq_resources.fill(0, coreset_nof_resources, true);

  // Number of symbols equal to CORESET#0 duration.
  const pdcch_type0_css_coreset_description desc = pdcch_type0_css_coreset_get(
      params.dl_carrier.band, *params.scs_ssb, params.scs_common, params.cs0_index->value(), params.k_ssb->value());

  return coreset_configuration{cs_id,
                               freq_resources,
                               desc.nof_symb_coreset,
                               std::nullopt,
                               coreset_configuration::precoder_granularity_type::same_as_reg_bundle};
}

/// Generates a default CORESET#0 configuration. The default CORESET#0 table index value used is equal to 6.
/// \remark See TS 38.213, Table 13-1.
coreset_configuration config_helpers::make_default_coreset0_config(const cell_config_builder_params_extended& params)
{
  return coreset_configuration{params.dl_carrier.band,
                               *params.scs_ssb,
                               params.scs_common,
                               *params.cs0_index,
                               *params.k_ssb,
                               *params.offset_to_point_a,
                               params.pci};
}

search_space_configuration
ocudu::config_helpers::make_default_search_space_zero_config(const cell_config_builder_params_extended& params)
{
  return search_space_configuration{
      params.dl_carrier.band, params.scs_common, *params.scs_ssb, *params.cs0_index, params.ss0_index};
}

search_space_configuration
ocudu::config_helpers::make_default_common_search_space_config(const cell_config_builder_params_extended& params)
{
  search_space_configuration::monitoring_symbols_within_slot_t monitoring_symbols_within_slot(
      NOF_OFDM_SYM_PER_SLOT_NORMAL_CP);
  monitoring_symbols_within_slot.set(0, true);
  return search_space_configuration{to_search_space_id(1),
                                    to_coreset_id(0),
                                    {0, 0, 1, 0, 0},
                                    search_space_configuration::common_dci_format{.f0_0_and_f1_0 = true},
                                    1,
                                    0,
                                    params.scs_common,
                                    1,
                                    monitoring_symbols_within_slot};
}

search_space_configuration
ocudu::config_helpers::make_default_ue_search_space_config(const cell_config_builder_params_extended& params)
{
  search_space_configuration cfg = make_default_common_search_space_config(params);
  cfg.set_non_ss0_coreset_id(to_coreset_id(1));
  cfg.set_non_ss0_id(to_search_space_id(2));
  cfg.set_non_ss0_monitored_dci_formats(search_space_configuration::ue_specific_dci_format::f0_1_and_1_1);
  return cfg;
}

bwp_configuration ocudu::config_helpers::make_default_init_bwp(const cell_config_builder_params_extended& params)
{
  bwp_configuration cfg{};
  cfg.scs  = params.scs_common;
  cfg.crbs = {0, params.cell_nof_crbs};
  cfg.cp   = cyclic_prefix::NORMAL;
  return cfg;
}

dl_config_common ocudu::config_helpers::make_default_dl_config_common(const cell_config_builder_params_extended& params)
{
  dl_config_common cfg{};

  // Configure FrequencyInfoDL.
  cfg.freq_info_dl.freq_band_list.emplace_back();
  cfg.freq_info_dl.freq_band_list.back().band = params.dl_carrier.band;
  cfg.freq_info_dl.offset_to_point_a          = params.offset_to_point_a->value();
  cfg.freq_info_dl.scs_carrier_list.emplace_back();
  cfg.freq_info_dl.scs_carrier_list.back().scs               = params.scs_common;
  cfg.freq_info_dl.scs_carrier_list.back().offset_to_carrier = 0;
  cfg.freq_info_dl.scs_carrier_list.back().carrier_bandwidth = params.cell_nof_crbs;

  cfg.freq_info_dl.absolute_frequency_ssb = params.ssb_arfcn.value();

  // \c params.dl_f_ref_arfcn refers to the ARFCN of the DL f_ref, as per TS 38.104, Section 5.4.2.1.
  const double dl_absolute_freq_point_a = band_helper::get_abs_freq_point_a_from_f_ref(
      band_helper::nr_arfcn_to_freq(params.dl_carrier.arfcn_f_ref), params.cell_nof_crbs, params.scs_common);
  // \c absolute_freq_point_a needs to be expressed as in ARFCN, as per \c absoluteFrequencyPointA definition in 38.211,
  // Section 4.4.4.2.
  cfg.freq_info_dl.absolute_freq_point_a = band_helper::freq_to_nr_arfcn(dl_absolute_freq_point_a);

  // Configure initial DL BWP.
  cfg.init_dl_bwp.generic_params = make_default_init_bwp(params);
  cfg.init_dl_bwp.pdcch_common.coreset0.emplace(make_default_coreset0_config(params));
  cfg.init_dl_bwp.pdcch_common.search_spaces.push_back(make_default_search_space_zero_config(params));
  cfg.init_dl_bwp.pdcch_common.search_spaces.push_back(make_default_common_search_space_config(params));
  cfg.init_dl_bwp.pdcch_common.sib1_search_space_id   = to_search_space_id(0);
  cfg.init_dl_bwp.pdcch_common.paging_search_space_id = to_search_space_id(1);
  cfg.init_dl_bwp.pdcch_common.ra_search_space_id     = to_search_space_id(1);
  cfg.init_dl_bwp.pdsch_common.pdsch_td_alloc_list = time_domain_resource_helper::generate_dedicated_pdsch_td_res_list(
      params.tdd_ul_dl_cfg_common,
      cfg.init_dl_bwp.generic_params.cp,
      time_domain_resource_helper::calculate_minimum_pdsch_symbol(cfg.init_dl_bwp.pdcch_common,
                                                                  make_ue_dedicated_pdcch_config(params)),
      params.enable_pdsch_mapping_type_b);

  // Configure PCCH.
  cfg.pcch_cfg.default_paging_cycle = paging_cycle::rf128;
  cfg.pcch_cfg.nof_pf               = pcch_config::nof_pf_per_drx_cycle::oneT;
  cfg.pcch_cfg.paging_frame_offset  = 0;
  cfg.pcch_cfg.ns                   = pcch_config::nof_po_per_pf::one;

  return cfg;
}

ul_config_common ocudu::config_helpers::make_default_ul_config_common(const cell_config_builder_params_extended& params)
{
  ul_config_common cfg{};
  // This is the ARFCN of the UL f_ref, as per TS 38.104, Section 5.4.2.1.
  const arfcn_t ul_arfcn =
      band_helper::get_ul_arfcn_from_dl_arfcn(params.dl_carrier.arfcn_f_ref, params.dl_carrier.band);
  const double ul_absolute_freq_point_a = band_helper::get_abs_freq_point_a_from_f_ref(
      band_helper::nr_arfcn_to_freq(ul_arfcn), params.cell_nof_crbs, params.scs_common);
  // \c absolute_freq_point_a needs to be expressed as in ARFCN, as per \c absoluteFrequencyPointA definition in 38.211,
  // Section 4.4.4.2.
  cfg.freq_info_ul.absolute_freq_point_a = band_helper::freq_to_nr_arfcn(ul_absolute_freq_point_a);
  cfg.freq_info_ul.scs_carrier_list.resize(1);
  cfg.freq_info_ul.scs_carrier_list[0].scs               = params.scs_common;
  cfg.freq_info_ul.scs_carrier_list[0].offset_to_carrier = 0;
  cfg.freq_info_ul.scs_carrier_list[0].carrier_bandwidth = params.cell_nof_crbs;
  cfg.freq_info_ul.freq_band_list.emplace_back();
  cfg.freq_info_ul.freq_band_list.back().band = params.dl_carrier.band;
  cfg.init_ul_bwp.generic_params              = make_default_init_bwp(params);
  cfg.init_ul_bwp.rach_cfg_common.emplace();
  cfg.init_ul_bwp.rach_cfg_common->rach_cfg_generic.zero_correlation_zone_config = 0;
  cfg.init_ul_bwp.rach_cfg_common->rach_cfg_generic.prach_config_index           = 16;
  if (band_helper::get_duplex_mode(params.dl_carrier.band) == duplex_mode::TDD) {
    std::optional<uint8_t> idx_found = prach_helper::find_valid_prach_config_index(
        params.scs_common,
        cfg.init_ul_bwp.rach_cfg_common->rach_cfg_generic.zero_correlation_zone_config,
        *params.tdd_ul_dl_cfg_common);
    ocudu_assert(idx_found.has_value(), "Unable to find a PRACH config index for the given TDD pattern");
    cfg.init_ul_bwp.rach_cfg_common->rach_cfg_generic.prach_config_index = idx_found.value();
  }

  // Although this is not specified in the TS, from our tests, the UE expects Msg1-SCS to be given when using short
  // PRACH Preambles formats. With long formats, we can set Msg1-SCS as \c invalid, in which case the UE derives the
  // PRACH SCS from \c prach-ConfigurationIndex in RACH-ConfigGeneric.
  const frequency_range freq_range = band_helper::get_freq_range(params.dl_carrier.band);
  const duplex_mode     duplex     = band_helper::get_duplex_mode(params.dl_carrier.band);
  cfg.init_ul_bwp.rach_cfg_common->msg1_scs =
      is_long_preamble(prach_configuration_get(
                           freq_range, duplex, cfg.init_ul_bwp.rach_cfg_common->rach_cfg_generic.prach_config_index)
                           .format)
          ? subcarrier_spacing::invalid
          : params.scs_common;
  cfg.init_ul_bwp.rach_cfg_common->restricted_set = restricted_set_config::UNRESTRICTED;
  // Set l839 for long preamble formats, l139 for short preamble formats, as per Tables 6.3.3.1-1 and 6.3.3.1-2,
  // TS 38.211
  cfg.init_ul_bwp.rach_cfg_common->is_prach_root_seq_index_l839 = is_long_preamble(
      prach_configuration_get(freq_range, duplex, cfg.init_ul_bwp.rach_cfg_common->rach_cfg_generic.prach_config_index)
          .format);
  cfg.init_ul_bwp.rach_cfg_common->prach_root_seq_index      = 1;
  cfg.init_ul_bwp.rach_cfg_common->rach_cfg_generic.msg1_fdm = 1;
  // Add +3 PRBS to the MSG1 frequency start, which act as a guardband between the PUCCH and PRACH.
  cfg.init_ul_bwp.rach_cfg_common->rach_cfg_generic.msg1_frequency_start = 6;

  cfg.init_ul_bwp.rach_cfg_common->rach_cfg_generic.ra_resp_window = 10U << to_numerology_value(params.scs_common);
  cfg.init_ul_bwp.rach_cfg_common->rach_cfg_generic.preamble_rx_target_pw = -100;

  cfg.init_ul_bwp.pusch_cfg_common.emplace();
  // - [Implementation-defined] Ensure k2 value which is less than or equal to minimum value of k1(s) exist in the
  // first entry of list. This way PDSCH(s) are scheduled before PUSCH and all DL slots are filled with PDSCH and all
  // UL slots are filled with PUSCH under heavy load. It also ensures that correct DAI value goes in the UL PDCCH of
  // DCI Format 0_1.
  cfg.init_ul_bwp.pusch_cfg_common->pusch_td_alloc_list =
      time_domain_resource_helper::generate_dedicated_pusch_td_res_list(
          params.tdd_ul_dl_cfg_common,
          cfg.init_ul_bwp.generic_params.cp,
          params.min_k2,
          params.enable_pusch_mapping_type_b);

  cfg.init_ul_bwp.pucch_cfg_common.emplace();
  cfg.init_ul_bwp.pucch_cfg_common->pucch_resource_common        = 11;
  cfg.init_ul_bwp.pucch_cfg_common->group_hopping                = pucch_group_hopping::NEITHER;
  cfg.init_ul_bwp.pucch_cfg_common->p0_nominal                   = -90;
  cfg.init_ul_bwp.pusch_cfg_common.value().p0_nominal_with_grant = -76;
  cfg.init_ul_bwp.pusch_cfg_common.value().msg3_delta_preamble   = 6;
  cfg.init_ul_bwp.pusch_cfg_common.value().msg3_delta_power      = 8;

  return cfg;
}

ssb_configuration ocudu::config_helpers::make_default_ssb_config(const cell_config_builder_params_extended& params)
{
  ssb_configuration cfg{};

  cfg.scs               = *params.scs_ssb;
  cfg.offset_to_point_A = *params.offset_to_point_a;
  cfg.ssb_period        = ssb_periodicity::ms10;
  cfg.k_ssb             = *params.k_ssb;

  // Set SSB idx 0 to 1.
  cfg.ssb_bitmap.set(0);
  cfg.ssb_bitmap.set_L_max(ssb_get_L_max(*params.scs_ssb, params.dl_carrier.arfcn_f_ref, params.dl_carrier.band));
  // Set SSB beam ID 0 (corresponding to SSB idx 0) to 0.
  cfg.beam_ids = {0};

  // The values we assign to these parameters are implementation-defined.
  cfg.ssb_block_power = -16;
  cfg.pss_to_sss_epre = ssb_pss_to_sss_epre::dB_0;

  return cfg;
}

pusch_config ocudu::config_helpers::make_default_pusch_config(const cell_config_builder_params_extended& params)
{
  // Default PUSCH transmission scheme is codebook with at maximum one layer. It assumes the UE Capability parameter
  // pusch-TransCoherence is nonCoherent.
  static constexpr unsigned                  max_rank        = 1;
  static constexpr tx_scheme_codebook_subset codebook_subset = tx_scheme_codebook_subset::non_coherent;

  pusch_config cfg{};
  cfg.tx_cfg = tx_scheme_codebook{.max_rank = max_rank, .codebook_subset = codebook_subset};
  cfg.pusch_mapping_type_a_dmrs.emplace();
  cfg.pusch_mapping_type_a_dmrs.value().trans_precoder_disabled.emplace();
  cfg.pusch_mapping_type_a_dmrs.value().additional_positions = dmrs_additional_positions::pos2;

  cfg.pusch_pwr_ctrl = pusch_config::pusch_power_control{.msg3_alpha               = alpha::alpha1,
                                                         .p0_nominal_without_grant = -76,
                                                         .p0_alphasets             = {},
                                                         .pathloss_ref_rs          = {},
                                                         .sri_pusch_mapping        = {}};
  cfg.pusch_pwr_ctrl.value().p0_alphasets.push_back(pusch_config::pusch_power_control::p0_pusch_alphaset{
      .id = static_cast<p0_pusch_alphaset_id>(0), .p0 = 0, .p0_pusch_alpha = alpha::alpha1});
  cfg.pusch_pwr_ctrl.value().pathloss_ref_rs.push_back(pusch_config::pusch_power_control::pusch_pathloss_ref_rs{
      .id = static_cast<pusch_config::pusch_power_control::pusch_pathloss_ref_rs_id>(0),
      .rs = static_cast<ssb_id_t>(0)});
  cfg.pusch_pwr_ctrl.value().sri_pusch_mapping.push_back(pusch_config::pusch_power_control::sri_pusch_pwr_ctrl{
      .id                           = static_cast<pusch_config::pusch_power_control::sri_pusch_pwr_ctrl_id>(0),
      .sri_pusch_pathloss_ref_rs_id = static_cast<pusch_config::pusch_power_control::pusch_pathloss_ref_rs_id>(0),
      .sri_p0_pusch_alphaset_id     = static_cast<p0_pusch_alphaset_id>(0),
      .closed_loop_idx = pusch_config::pusch_power_control::sri_pusch_pwr_ctrl::sri_pusch_closed_loop_index::i0});
  cfg.res_alloc      = pusch_config::resource_allocation::resource_allocation_type_1;
  cfg.trans_precoder = pusch_config::transform_precoder::disabled;

  cfg.uci_cfg.emplace();
  auto& uci_cfg   = cfg.uci_cfg.value();
  uci_cfg.scaling = alpha_scaling_opt::f1;
  beta_offsets b_offset{};
  b_offset.beta_offset_ack_idx_1    = 9;
  b_offset.beta_offset_ack_idx_2    = 9;
  b_offset.beta_offset_ack_idx_3    = 9;
  b_offset.beta_offset_csi_p1_idx_1 = 9;
  b_offset.beta_offset_csi_p1_idx_2 = 9;
  b_offset.beta_offset_csi_p2_idx_1 = 9;
  b_offset.beta_offset_csi_p2_idx_2 = 9;
  uci_cfg.beta_offsets_cfg          = uci_on_pusch::beta_offsets_semi_static{b_offset};

  cfg.harq_process_num_size_dci_0_1 = pusch_config::harq_process_num_dci_0_1_size::n4;
  cfg.harq_process_num_size_dci_0_2 = pusch_config::harq_process_num_dci_0_2_size::n4;

  return cfg;
}

srs_config ocudu::config_helpers::make_default_srs_config(const cell_config_builder_params_extended& params)
{
  srs_config cfg{};

  cfg.srs_res_list.emplace_back();
  // TODO: Verify correctness of the config based on what we support.
  srs_config::srs_resource& res    = cfg.srs_res_list.back();
  res.id.ue_res_id                 = static_cast<srs_config::srs_res_id>(0);
  res.nof_ports                    = srs_config::srs_resource::nof_srs_ports::port1;
  res.tx_comb.size                 = tx_comb_size::n2;
  res.tx_comb.tx_comb_offset       = 0;
  res.tx_comb.tx_comb_cyclic_shift = 0;
  res.res_mapping.start_pos        = 0;
  res.res_mapping.nof_symb         = srs_nof_symbols::n1;
  res.res_mapping.rept_factor      = srs_nof_symbols::n1;
  res.freq_domain_pos              = 0;
  res.freq_domain_shift            = 0;
  // NOTE: C_SRS, B_SRS and B_hop are chosen to disable SRS frequency hopping and to monitor SRS over smallest
  // possible BW i.e. 4 RBs. See TS 38.211, Table 6.4.1.4.3-1.
  // This is done to cater to setups of all BWs until SRS is supported in scheduler.
  // TODO: Support SRS in scheduler.
  res.freq_hop.c_srs = 0;
  res.freq_hop.b_srs = 0;
  res.freq_hop.b_hop = 0;
  res.grp_or_seq_hop = srs_group_or_sequence_hopping::neither;
  res.res_type       = srs_resource_type::aperiodic;
  res.sequence_id    = params.pci;

  cfg.srs_res_set_list.emplace_back();
  // TODO: Verify correctness of the config based on what we support.
  srs_config::srs_resource_set& res_set = cfg.srs_res_set_list.back();
  res_set.id                            = static_cast<srs_config::srs_res_set_id>(0);
  res_set.srs_res_id_list.emplace_back(static_cast<srs_config::srs_res_id>(0));
  res_set.res_type =
      srs_config::srs_resource_set::aperiodic_resource_type{.aperiodic_srs_res_trigger = 1, .slot_offset = 7};

  res_set.srs_res_set_usage = srs_usage::codebook;
  res_set.p0                = -84;
  res_set.pathloss_ref_rs   = static_cast<ssb_id_t>(0);

  return cfg;
}

uplink_config ocudu::config_helpers::make_default_ue_uplink_config(const cell_config_builder_params_extended& params)
{
  // > UL Config.
  uplink_config ul_config{};

  // >> PUCCH.
  auto& pucch_cfg = ul_config.init_ul_bwp.pucch_cfg.emplace();

  // Get the default PUCCH cell resource list.
  pucch_resource_builder_params builder_params{};
  const auto cell_pucch_res_list = config_helpers::build_pucch_resource_list(builder_params, params.cell_nof_crbs);

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
    static constexpr unsigned default_sr_period_ms = 40;
    const unsigned            sr_period = get_nof_slots_per_subframe(params.scs_common) * default_sr_period_ms;
    const unsigned            sr_offset =
        params.tdd_ul_dl_cfg_common.has_value() ? find_next_tdd_full_ul_slot(*params.tdd_ul_dl_cfg_common).value() : 0;
    auto& sr_res = pucch_cfg.pucch_res_list.emplace_back(cell_pucch_res_list[builder_params.res_set_0_size.value()]);
    sr_res.res_id.ue_res_id = pucch_cfg.pucch_res_list.size() - 1;
    pucch_cfg.sr_res_list.push_back(scheduling_request_resource_config{.sr_res_id = 1,
                                                                       .sr_id     = uint_to_sched_req_id(0),
                                                                       .period = static_cast<sr_periodicity>(sr_period),
                                                                       .offset = sr_offset,
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
  {
    const unsigned csi_cell_res_offset =
        builder_params.res_set_0_size.value() * builder_params.nof_cell_res_set_configs +
        builder_params.nof_cell_sr_resources +
        builder_params.res_set_1_size.value() * builder_params.nof_cell_res_set_configs;
    auto& res            = pucch_cfg.pucch_res_list.emplace_back(cell_pucch_res_list[csi_cell_res_offset]);
    res.res_id.ue_res_id = pucch_cfg.pucch_res_list.size() - 1;
  }

  // Increase code rate in case of more than 4 layers.
  const unsigned nof_dl_layers =
      params.max_nof_layers.has_value() ? params.max_nof_layers.value() : params.dl_carrier.nof_ant;
  const max_pucch_code_rate max_c_rate = nof_dl_layers >= 4 ? max_pucch_code_rate::dot_35 : max_pucch_code_rate::dot_25;

  pucch_cfg.format_1_common_param.emplace();
  pucch_cfg.format_2_common_param.emplace(
      pucch_common_all_formats{.max_c_rate = max_c_rate, .simultaneous_harq_ack_csi = true});
  pucch_cfg.format_3_common_param.emplace(
      pucch_common_all_formats{.max_c_rate = max_c_rate, .simultaneous_harq_ack_csi = true});
  pucch_cfg.format_4_common_param.emplace(
      pucch_common_all_formats{.max_c_rate = max_c_rate, .simultaneous_harq_ack_csi = true});
  pucch_cfg.dl_data_to_ul_ack =
      time_domain_resource_helper::generate_k1_candidates(params.tdd_ul_dl_cfg_common, params.min_k1);

  // Compute the max UCI payload per format.
  // As per TS 38.231, Section 9.2.1, with PUCCH Format 1, we can have up to 2 HARQ-ACK bits (SR doesn't count as part
  // of the payload).
  {
    static constexpr unsigned pucch_f1_max_harq_payload                        = 2U;
    pucch_cfg.format_max_payload[pucch_format_to_uint(pucch_format::FORMAT_1)] = pucch_f1_max_harq_payload;

    const pucch_resource& res_f2        = pucch_cfg.pucch_res_list.back();
    const auto&           res_f2_params = std::get<pucch_format_2_3_cfg>(res_f2.format_params);
    pucch_cfg.format_max_payload[pucch_format_to_uint(pucch_format::FORMAT_2)] =
        get_pucch_format2_max_payload(res_f2_params.nof_prbs,
                                      res_f2.nof_symbols,
                                      to_max_code_rate_float(pucch_cfg.format_2_common_param.value().max_c_rate));
    pucch_cfg.set_1_format = pucch_format::FORMAT_2;
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

  // > PUSCH config.
  ul_config.init_ul_bwp.pusch_cfg.emplace(make_default_pusch_config());

  // > pusch-ServingCellConfig.
  ul_config.pusch_serv_cell_cfg.emplace();

  // > SRS config.
  ul_config.init_ul_bwp.srs_cfg.emplace(make_default_srs_config(params));

  return ul_config;
}

pdsch_serving_cell_config ocudu::config_helpers::make_default_pdsch_serving_cell_config()
{
  pdsch_serving_cell_config serv_cell;
  serv_cell.nof_harq_proc   = pdsch_serving_cell_config::nof_harq_proc_for_pdsch::n16;
  serv_cell.max_mimo_layers = 1;

  return serv_cell;
}

csi_helper::csi_meas_config_builder_params
ocudu::config_helpers::make_default_csi_builder_params(const cell_config_builder_params_extended& params)
{
  // Parameters used to generate list of CSI resources.
  csi_helper::csi_meas_config_builder_params csi_params{};
  csi_params.pci                      = params.pci;
  csi_params.nof_rbs                  = params.cell_nof_crbs;
  csi_params.nof_ports                = params.dl_carrier.nof_ant;
  csi_params.max_nof_layers           = *params.max_nof_layers;
  csi_params.mcs_table                = pdsch_mcs_table::qam64;
  csi_params.csi_params.csi_rs_period = csi_helper::get_max_csi_rs_period(params.scs_common);

  if (band_helper::get_duplex_mode(params.dl_carrier.band) == duplex_mode::TDD) {
    // Set a default CSI report slot offset that falls in an UL slot.
    const auto& tdd_pattern = *params.tdd_ul_dl_cfg_common;

    constexpr unsigned default_ssb_period_ms = 10U;

    const unsigned max_csi_symbol = *std::max_element(csi_params.csi_params.tracking_csi_ofdm_symbol_indices.begin(),
                                                      csi_params.csi_params.tracking_csi_ofdm_symbol_indices.end());

    if (not csi_helper::derive_valid_csi_rs_slot_offsets(csi_params.csi_params,
                                                         std::nullopt,
                                                         std::nullopt,
                                                         std::nullopt,
                                                         tdd_pattern,
                                                         max_csi_symbol,
                                                         default_ssb_period_ms)) {
      report_fatal_error("Failed to find valid csi-MeasConfig");
    }

    for (unsigned i = 0; i != nof_slots_per_tdd_period(tdd_pattern); ++i) {
      // TODO: Support reports in the special slot.
      if (is_tdd_full_ul_slot(tdd_pattern, i)) {
        csi_params.csi_params.csi_report_slot_offset = i;
      }
    }
  }

  return csi_params;
}

pdsch_config ocudu::config_helpers::make_default_pdsch_config(const cell_config_builder_params_extended& params)
{
  pdsch_config pdsch_cfg;
  pdsch_cfg.pdsch_mapping_type_a_dmrs.emplace();
  dmrs_downlink_config& dmrs_type_a = pdsch_cfg.pdsch_mapping_type_a_dmrs.value();
  dmrs_type_a.additional_positions  = dmrs_additional_positions::pos2;
  if (params.enable_pdsch_mapping_type_b) {
    pdsch_cfg.pdsch_mapping_type_b_dmrs.emplace();
    dmrs_downlink_config& dmrs_type_b = pdsch_cfg.pdsch_mapping_type_b_dmrs.value();
    dmrs_type_b.additional_positions  = dmrs_additional_positions::pos2;
  }
  pdsch_cfg.tci_states.push_back(tci_state{
      .state_id  = static_cast<tci_state_id_t>(0),
      .qcl_type1 = {.ref_sig  = {.type = qcl_info::reference_signal::reference_signal_type::ssb,
                                 .ssb  = static_cast<ssb_id_t>(0)},
                    .qcl_type = qcl_info::qcl_type::type_d},
  });
  pdsch_cfg.res_alloc = pdsch_config::resource_allocation::resource_allocation_type_1;
  pdsch_cfg.rbg_sz    = rbg_size::config1;
  pdsch_cfg.prb_bndlg.bundling.emplace<prb_bundling::static_bundling>(
      prb_bundling::static_bundling({.sz = prb_bundling::static_bundling::bundling_size::wideband}));

  pdsch_cfg.vrb_to_prb_interleaving = vrb_to_prb::mapping_type::non_interleaved;

  if (params.csi_rs_enabled) {
    const csi_helper::csi_meas_config_builder_params csi_params = make_default_csi_builder_params(params);

    // zp-CSI-RS resources.
    pdsch_cfg.zp_csi_rs_res_list = csi_helper::make_periodic_zp_csi_rs_resource_list(csi_params);

    // periodic set of zp-CSI-RS resources.
    pdsch_cfg.p_zp_csi_rs_res = csi_helper::make_periodic_zp_csi_rs_resource_set(csi_params);
  }

  pdsch_cfg.harq_process_num_size_dci_1_1 = pdsch_config::harq_process_num_dci_1_1_size::n4;
  pdsch_cfg.harq_process_num_size_dci_1_2 = pdsch_config::harq_process_num_dci_1_2_size::n4;

  return pdsch_cfg;
}

pdcch_config ocudu::config_helpers::make_ue_dedicated_pdcch_config(const cell_config_builder_params_extended& params)
{
  pdcch_config pdcch_cfg{};
  // >> Add CORESET#1.
  pdcch_cfg.coresets.push_back(make_default_coreset_config(params));
  // >> Add SearchSpace#2.
  pdcch_cfg.search_spaces.push_back(make_default_ue_search_space_config());
  pdcch_cfg.search_spaces[0].set_non_ss0_nof_candidates(
      {0,
       compute_max_nof_candidates(aggregation_level::n2, pdcch_cfg.coresets[0]),
       compute_max_nof_candidates(aggregation_level::n4, pdcch_cfg.coresets[0]),
       0,
       0});

  return pdcch_cfg;
}

csi_meas_config ocudu::config_helpers::make_csi_meas_config(const cell_config_builder_params_extended& params)
{
  // Parameters used to generate list of CSI resources.
  const csi_helper::csi_meas_config_builder_params csi_params = make_default_csi_builder_params(params);

  // Generate CSI resources.
  // Note: Since by default we use periodic CSI, we pass an empty PUSCH TD allocation list.
  return csi_helper::make_csi_meas_config(csi_params, {});
}

serving_cell_config
ocudu::config_helpers::create_default_initial_ue_serving_cell_config(const cell_config_builder_params_extended& params)
{
  serving_cell_config serv_cell;
  serv_cell.cell_index = to_du_cell_index(0);

  // > PDCCH-Config.
  serv_cell.init_dl_bwp.pdcch_cfg.emplace(make_ue_dedicated_pdcch_config(params));

  // > PDSCH-Config.
  serv_cell.init_dl_bwp.pdsch_cfg = make_default_pdsch_config(params);

  // > UL Config.
  serv_cell.ul_config.emplace(make_default_ue_uplink_config(params));

  // > pdsch-ServingCellConfig.
  serv_cell.pdsch_serv_cell_cfg.emplace(make_default_pdsch_serving_cell_config());

  // > CSI-MeasConfig.
  if (params.csi_rs_enabled) {
    // Generate CSI resources.
    serv_cell.csi_meas_cfg = make_csi_meas_config(params);
  }

  return serv_cell;
}

ue_cell_config
ocudu::config_helpers::create_default_initial_ue_cell_config(const cell_config_builder_params_extended& params)
{
  ue_cell_config cfg{};

  cfg.serv_cell_cfg = create_default_initial_ue_serving_cell_config(params);

  auto& init_bwp = cfg.bwps.emplace_back();
  if (params.csi_rs_enabled) {
    const csi_helper::csi_meas_config_builder_params csi_params = make_default_csi_builder_params(params);
    init_bwp.ul.periodic_csi_report.emplace(ue_periodic_csi_config{
        .pucch_res_id = pucch_csi_resource_id(0), .offset = csi_params.csi_params.csi_report_slot_offset.value()});
  }
  init_bwp.ul.pucch.res_set_cfg_id = pucch_resource_set_config_id(0);
  init_bwp.ul.pucch.sr_res_id      = pucch_sr_resource_id(0);
  init_bwp.ul.pucch.sr_offset =
      params.tdd_ul_dl_cfg_common.has_value() ? find_next_tdd_full_ul_slot(*params.tdd_ul_dl_cfg_common).value() : 0;
  init_bwp.ul.pusch.tx_cfg = {.max_rank = 1, .codebook_subset = tx_scheme_codebook_subset::non_coherent};

  return cfg;
}

uint8_t ocudu::config_helpers::compute_max_nof_candidates(aggregation_level            aggr_lvl,
                                                          const coreset_configuration& cs_cfg)
{
  // 1 REG = 1 RB and 1 symbol.
  // 1 CCE = 6 {PRB, symbol}. e.g. 3 PRBs over 2 symbols or 6 PRBs over 1 symbol, etc.
  // Example: 3 Frequency domain resources, 2 symbol duration contains 6 CCEs.
  const unsigned max_coreset_cces   = cs_cfg.freq_domain_resources().count() * cs_cfg.duration();
  unsigned       max_nof_candidates = max_coreset_cces / to_nof_cces(aggr_lvl);
  // See TS 38.331, SearchSpace IE.
  // aggregationLevelX - ENUMERATED {n0, n1, n2, n3, n4, n5, n6, n8}.
  if (max_nof_candidates == 7) {
    max_nof_candidates = 6;
  }
  return max_nof_candidates > PDCCH_MAX_NOF_CANDIDATES_SS ? PDCCH_MAX_NOF_CANDIDATES_SS : max_nof_candidates;
}
