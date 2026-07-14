// SPDX-FileCopyrightText: Copyright (C) 2021-2026 Software Radio Systems Limited
// SPDX-License-Identifier: BSD-3-Clause-Open-MPI
// Portions of this file may implement 3GPP specifications, which may be subject to additional licensing requirements.

#include "apps/units/o_cu_cp/cu_cp/cu_cp_remote_commands.h"
#include "nlohmann/json.hpp"
#include "ocudu/ran/pci.h"
#include "ocudu/ran/plmn_identity.h"
#include "ocudu/ran/rnti.h"
#include "ocudu/ran/tac.h"

using namespace ocudu;

static expected<uint64_t, std::string> parse_unsigned_field(const nlohmann::json& json, const char* name)
{
  auto key = json.find(name);
  if (key == json.end()) {
    return make_unexpected(fmt::format("'{}' object is missing and it is mandatory", name));
  }
  if (!key->is_number_unsigned()) {
    return make_unexpected(fmt::format("'{}' object value type should be an unsigned integer", name));
  }

  return key->get<uint64_t>();
}

static expected<pci_t, std::string> parse_pci_field(const nlohmann::json& json, const char* name)
{
  expected<uint64_t, std::string> value = parse_unsigned_field(json, name);
  if (not value.has_value()) {
    return make_unexpected(value.error());
  }
  if (value.value() > MAX_PCI) {
    return make_unexpected(fmt::format(
        "'{}' value out of range, received '{}', valid range is from {} to {}", name, value.value(), MIN_PCI, MAX_PCI));
  }

  return static_cast<pci_t>(value.value());
}

error_type<std::string> handover_remote_command::execute(const nlohmann::json& json)
{
  expected<pci_t, std::string> serving_pci = parse_pci_field(json, "serving_pci");
  if (not serving_pci.has_value()) {
    return make_unexpected(serving_pci.error());
  }

  expected<uint64_t, std::string> rnti = parse_unsigned_field(json, "rnti");
  if (not rnti.has_value()) {
    return make_unexpected(rnti.error());
  }
  if (rnti.value() == 0U || rnti.value() > 0xffffU) {
    return make_unexpected(
        fmt::format("'rnti' value out of range, received '{}', valid range is from 1 to 65535", rnti.value()));
  }

  expected<pci_t, std::string> target_pci = parse_pci_field(json, "target_pci");
  if (not target_pci.has_value()) {
    return make_unexpected(target_pci.error());
  }

  auto target_plmn_key = json.find("target_plmn");
  if (target_plmn_key == json.end()) {
    return make_unexpected("'target_plmn' object is missing and it is mandatory");
  }
  if (!target_plmn_key->is_string()) {
    return make_unexpected("'target_plmn' object value type should be a string");
  }
  expected<plmn_identity> target_plmn =
      plmn_identity::parse(target_plmn_key.value().get_ref<const nlohmann::json::string_t&>());
  if (not target_plmn.has_value()) {
    return make_unexpected(fmt::format("Invalid 'target_plmn' value '{}'. Expected 5 or 6 digits",
                                       target_plmn_key.value().get_ref<const nlohmann::json::string_t&>()));
  }

  expected<uint64_t, std::string> target_tac = parse_unsigned_field(json, "target_tac");
  if (not target_tac.has_value()) {
    return make_unexpected(target_tac.error());
  }
  if (target_tac.value() == 0U || target_tac.value() == 0xfffffeU) {
    return make_unexpected(fmt::format(
        "Invalid 'target_tac' {}. Values 0 and 16777214 (0xfffffe) are reserved", target_tac.value()));
  }
  if (target_tac.value() >= INVALID_TAC) {
    return make_unexpected(
        fmt::format("'target_tac' value out of range, received '{}', valid range is from 0 to {}",
                    target_tac.value(),
                    INVALID_TAC - 1));
  }

  cu_cp.get_mobility_command_handler().trigger_handover(serving_pci.value(),
                                                        static_cast<rnti_t>(rnti.value()),
                                                        target_pci.value(),
                                                        target_plmn.value(),
                                                        static_cast<tac_t>(target_tac.value()));

  return {};
}
