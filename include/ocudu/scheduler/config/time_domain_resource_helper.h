// SPDX-FileCopyrightText: Copyright (C) 2021-2026 Software Radio Systems Limited
// SPDX-License-Identifier: BSD-3-Clause-Open-MPI
// Portions of this file may implement 3GPP specifications, which may be subject to additional licensing requirements.

#pragma once

#include "ocudu/adt/static_vector.h"
#include "ocudu/ran/pdsch/pdsch_time_domain_resource.h"
#include "ocudu/ran/pusch/pusch_time_domain_resource.h"
#include "ocudu/ran/tdd/tdd_ul_dl_config.h"
#include <vector>

namespace ocudu {

struct pdcch_config_common;
struct pdcch_config;

namespace time_domain_resource_helper {

/// \brief Generate the list of available k1 candidates for PDSCH-to-HARQ timing for TDD operation.
static_vector<uint8_t, 8> generate_k1_candidates(const tdd_ul_dl_config_common& tdd_cfg, uint8_t min_k1);
static_vector<uint8_t, 8> generate_k1_candidates(const std::optional<tdd_ul_dl_config_common>& tdd_cfg, uint8_t min_k1);

/// \brief Computes the minimum symbol available for PDSCH, considering that they won't collide with the symbols
/// reserved for PDCCH in the slot.
/// \param[in] common_pdcch_cfg Common PDCCH configuration.
/// \param[in] ded_pdcch_cfg UE dedicated PDCCH configuration.
uint8_t calculate_minimum_pdsch_symbol(const pdcch_config_common&         common_pdcch_cfg,
                                       const std::optional<pdcch_config>& ded_pdcch_cfg);

/// \brief Creates PDSCH Time Domain Resource allocation list considering the symbols available for PDSCH.
/// \param[in] tdd_cfg TDD configuration.
/// \param[in] cp Cyclic prefix length.
/// \param[in] min_pdsch_symbol Minimum symbol available for PDSCH. This value is derived based on the configured
/// CORESETs and search spaces.
/// \param enable_mapping_type_b When true, additionally emits SHORT PDSCH mapping type B rows.
/// TS 38.214 Table 5.1.2.1-1, Section 5.1.2.1
/// \return List of PDSCH Time Domain Resource allocation.
/// \remark The algorithm to choose the candidates is implementation defined.
std::vector<pdsch_time_domain_resource_allocation>
generate_dedicated_pdsch_td_res_list(const tdd_ul_dl_config_common& tdd_cfg,
                                     cyclic_prefix                  cp,
                                     uint8_t                        min_pdsch_symbol,
                                     bool                           enable_mapping_type_b = false);
std::vector<pdsch_time_domain_resource_allocation>
generate_dedicated_pdsch_td_res_list(const std::optional<tdd_ul_dl_config_common>& tdd_cfg,
                                     cyclic_prefix                                 cp,
                                     uint8_t                                       min_pdsch_symbol,
                                     bool                                          enable_mapping_type_b = false);

/// \brief Generate the list of available UE dedicated PUSCH time-domain resource allocations for TDD operation.
/// \remark The algorithm to choose the candidates is implementation defined.
///
/// \param enable_mapping_type_b When true, additionally emits PUSCH mapping type B rows targeting the PARTIAL uplink
/// slots of the TDD pattern, i.e. the trailing UL symbols of a special slot.
/// TS 38.213 Section 11.1, TS 38.214 Table 6.1.2.1-1, Section 6.1.2.1
std::vector<pusch_time_domain_resource_allocation> generate_dedicated_pusch_td_res_list(
    const tdd_ul_dl_config_common& tdd_cfg,
    cyclic_prefix                  cp,
    uint8_t                        min_k2,
    bool                           enable_mapping_type_b = false);
std::vector<pusch_time_domain_resource_allocation>
generate_dedicated_pusch_td_res_list(const std::optional<tdd_ul_dl_config_common>& tdd_cfg,
                                     cyclic_prefix                                 cp,
                                     uint8_t                                       min_k2,
                                     bool                                          enable_mapping_type_b = false);

} // namespace time_domain_resource_helper

} // namespace ocudu
