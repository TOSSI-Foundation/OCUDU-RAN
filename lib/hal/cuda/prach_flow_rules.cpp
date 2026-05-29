#include "ocudu/hal/cuda/prach_flow_rules.h"

#include <cstdio>
#include <cstring>

#include <rte_byteorder.h>
#include <rte_ecpri.h>
#include <rte_ethdev.h>
#include <rte_flow.h>

namespace ocudu {
namespace hal {
namespace cuda {

namespace {

::rte_flow*
build_rule_for_eaxc(uint16_t port_id, uint16_t , uint16_t queue_id, ::rte_flow_error& err)
{

  ::rte_flow_item_eth eth_spec{};
  ::rte_flow_item_eth eth_mask{};
  eth_spec.hdr.ether_type = rte_cpu_to_be_16(0xAEFE);
  eth_mask.hdr.ether_type = 0xFFFF;

  ::rte_flow_item pattern[] = {
      {.type = RTE_FLOW_ITEM_TYPE_ETH, .spec = &eth_spec, .last = nullptr, .mask = &eth_mask},
      {.type = RTE_FLOW_ITEM_TYPE_END, .spec = nullptr,   .last = nullptr, .mask = nullptr},
  };

  ::rte_flow_action_queue queue_action{};
  queue_action.index = queue_id;

  ::rte_flow_action actions[] = {
      {.type = RTE_FLOW_ACTION_TYPE_QUEUE, .conf = &queue_action},
      {.type = RTE_FLOW_ACTION_TYPE_END,   .conf = nullptr},
  };

  ::rte_flow_attr attr{};
  attr.ingress = 1;
  attr.priority = 0;

  if (::rte_flow_validate(port_id, &attr, pattern, actions, &err) != 0) {
    std::fprintf(stderr,
                 "[prach_flow] rte_flow_validate failed: type=%d msg=%s\n",
                 err.type,
                 err.message ? err.message : "(none)");
    return nullptr;
  }

  ::rte_flow* handle = ::rte_flow_create(port_id, &attr, pattern, actions, &err);
  if (handle == nullptr) {
    std::fprintf(stderr,
                 "[prach_flow] rte_flow_create failed: type=%d msg=%s\n",
                 err.type,
                 err.message ? err.message : "(none)");
  }
  return handle;
}

}

std::vector<::rte_flow*> install_prach_steering_rules(uint16_t                     port_id,
                                                      const std::vector<uint16_t>& prach_eaxcs,
                                                      uint16_t                     target_queue_id)
{

  std::vector<::rte_flow*> rules;

  ::rte_flow_error err{};
  ::rte_flow*      h = build_rule_for_eaxc(port_id,  0, target_queue_id, err);
  if (h == nullptr) {
    return {};
  }
  rules.push_back(h);
  std::fprintf(stderr,
               "[prach_flow] installed single eCPRI -> queue=%u rule "
               "(software-filtering for eAxC ∈ {%zu values})\n",
               target_queue_id,
               prach_eaxcs.size());
  return rules;
}

void destroy_flow_rules(uint16_t port_id, std::vector<::rte_flow*>& rules)
{
  for (::rte_flow* h : rules) {
    if (h != nullptr) {
      ::rte_flow_error err{};
      ::rte_flow_destroy(port_id, h, &err);
    }
  }
  rules.clear();
}

}
}
}
