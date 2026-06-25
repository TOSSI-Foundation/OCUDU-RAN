// SPDX-FileCopyrightText: Copyright (C) 2021-2026 Software Radio Systems Limited
// SPDX-License-Identifier: BSD-3-Clause-Open-MPI
// Portions of this file may implement 3GPP specifications, which may be subject to additional licensing requirements.

#pragma once

#include "ocudu/cu_cp/cu_cp_configuration.h"
#include "ocudu/ran/gnb_id.h"
#include "ocudu/ran/guami.h"
#include "ocudu/ran/plmn_identity.h"
#include "ocudu/ran/s_nssai.h"
#include <algorithm>
#include <chrono>
#include <string>
#include <vector>

namespace ocudu {

namespace ocucp {

/// \brief NGAP context
struct ngap_context_t {
  gnb_id_t                             gnb_id = {0, 22};
  std::string                          ran_node_name;
  std::string                          amf_name;
  amf_index_t                          amf_index;
  std::vector<supported_tracking_area> supported_tas;
  std::vector<guami_t>                 served_guami_list;
  uint16_t                             default_paging_drx = 256;    // default paging drx
  std::chrono::seconds                 request_pdu_session_timeout; // timeout for requesting a PDU session in seconds
  byte_buffer                          lmf_routing_id;

  std::vector<plmn_identity> get_supported_plmns() const
  {
    std::vector<plmn_identity> supported_plmns;
    for (const auto& ta : supported_tas) {
      for (const auto& plmn : ta.plmn_list) {
        supported_plmns.push_back(plmn.plmn_id);
      }
    }

    return supported_plmns;
  }

  std::vector<s_nssai_t> get_supported_snssais() const
  {
    std::vector<s_nssai_t> snssais;
    for (const auto& ta : supported_tas) {
      for (const auto& plmn : ta.plmn_list) {
        for (const auto& snssai : plmn.slice_support_list) {
          if (std::find(snssais.begin(), snssais.end(), snssai) == snssais.end()) {
            snssais.push_back(snssai);
          }
        }
      }
    }
    return snssais;
  }
};

} // namespace ocucp

} // namespace ocudu
