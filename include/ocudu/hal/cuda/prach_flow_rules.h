#pragma once

#include <cstdint>
#include <vector>

struct rte_flow;

namespace ocudu {
namespace hal {
namespace cuda {

std::vector<::rte_flow*> install_prach_steering_rules(uint16_t                     port_id,
                                                      const std::vector<uint16_t>& prach_eaxcs,
                                                      uint16_t                     target_queue_id);

void destroy_flow_rules(uint16_t port_id, std::vector<::rte_flow*>& rules);

}
}
}
