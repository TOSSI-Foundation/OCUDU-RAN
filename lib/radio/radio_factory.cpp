// SPDX-FileCopyrightText: Copyright (C) 2021-2026 Software Radio Systems Limited
// SPDX-License-Identifier: BSD-3-Clause-Open-MPI

#include "ocudu/radio/radio_factory.h"

#ifdef ENABLE_UHD
#include "uhd/radio_uhd_impl.h"
#endif // ENABLE_UHD

#ifdef ENABLE_ZMQ
#include "zmq/radio_factory_zmq_impl.h"
#endif // ENABLE_ZMQ

#ifdef ENABLE_SIONNA
#include "sionna/radio_factory_sionna_impl.h"
#endif // ENABLE_SIONNA

#include "plugin_radio_factory.h"

using namespace ocudu;

namespace {

struct radio_factory_entry {
  std::string                                     name;
  std::function<std::unique_ptr<radio_factory>()> make;
};

} // namespace

static const std::vector<radio_factory_entry> radio_factory_available_factories = {
#ifdef ENABLE_UHD
    {"uhd", []() { return std::make_unique<radio_factory_uhd_impl>(); }},
#endif // ENABLE_UHD
#ifdef ENABLE_ZMQ
    {"zmq", []() { return std::make_unique<radio_factory_zmq_impl>(); }},
#endif // ENABLE_ZMQ
#ifdef ENABLE_SIONNA
    {"sionna", []() { return std::make_unique<radio_factory_sionna_impl>(); }},
#endif // ENABLE_SIONNA
};

void ocudu::print_available_radio_factories()
{
  if (radio_factory_available_factories.empty()) {
    return;
  }

  // Print available factories.
  fmt::print("Available radio types: ");
  for (unsigned i = 0, e = radio_factory_available_factories.size(); i != e; ++i) {
    if (i > 0) {
      if (i == radio_factory_available_factories.size() - 1) {
        fmt::print(" and ");
      } else {
        fmt::print(", ");
      }
    }
    fmt::print("{}", radio_factory_available_factories[i].name);
  }
  fmt::print(".\n");
}

std::unique_ptr<radio_factory> ocudu::create_radio_factory(std::string driver_name)
{
  // Convert driver name to lower case.
  for (char& c : driver_name) {
    c = std::tolower(c);
  }

  // Iterate all available driver names.
  for (const radio_factory_entry& entry : radio_factory_available_factories) {
    if (entry.name == driver_name) {
      return entry.make();
    }
  }

  // Try creating a plugin radio factory.
  auto factory = create_plugin_radio_factory(driver_name);
  if (factory) {
    return factory;
  }

  // No match, print available factories.
  fmt::print("Factory for radio type {} not found. Make sure to select a valid type.\n", driver_name);

  return nullptr;
}
