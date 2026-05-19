#pragma once

#include "lib/ipc/xsm/xsm_context.h"
#include "lib/fapi/serialization/fapi_message_type_id.h"
#include "lib/fapi/serialization/fapi_serialization_buffer.h"
#include "lib/fapi/serialization/fapi_serializer_p5.h"
#include "lib/fapi/serialization/fapi_serializer_p7_dl.h"
#include "lib/fapi/serialization/fapi_serializer_p7_ind.h"
#include "lib/fapi/serialization/fapi_serializer_p7_ul.h"
#include "lib/fapi/serialization/fapi_xsm_message_header.h"
#include "ocudu/fapi/common/error_indication_notifier.h"
#include "ocudu/fapi/p5/p5_requests_gateway.h"
#include "ocudu/fapi/p5/p5_responses_notifier.h"
#include "ocudu/fapi/p7/p7_indications_notifier.h"
#include "ocudu/fapi/p7/p7_last_request_notifier.h"
#include "ocudu/fapi/p7/p7_requests_gateway.h"
#include "ocudu/fapi/p7/p7_slot_indication_notifier.h"
#include <array>
#include <atomic>
#include <cstdint>
#include <thread>
#include <vector>

namespace ocudu {


class fapi_xsm_transport
{
public:
  fapi_xsm_transport() = default;
  ~fapi_xsm_transport();

  fapi_xsm_transport(const fapi_xsm_transport&)            = delete;
  fapi_xsm_transport& operator=(const fapi_xsm_transport&) = delete;

  void init_l2_side();
  void init_l1_side();
  void shutdown();

  void set_rx_cpu(int cpu_id) { rx_cpu_id_ = cpu_id; }

  void set_rx_priority(int prio) { rx_priority_ = prio; }

  void start_receiver();

  void stop_receiver();

  xsm_context& get_xsm_context() { return xsm_; }

  void set_l2_p7_indications_notifier(fapi::p7_indications_notifier* n) { l2_p7_ind_notifier_ = n; }
  void set_l2_p7_slot_notifier(fapi::p7_slot_indication_notifier* n)    { l2_slot_notifier_ = n; }
  void set_l2_p5_responses_notifier(fapi::p5_responses_notifier* n)     { l2_p5_resp_notifier_ = n; }
  void set_l2_error_notifier(fapi::error_indication_notifier* n)        { l2_error_notifier_ = n; }

  void set_l1_p5_requests_gateway(fapi::p5_requests_gateway* g) { l1_p5_gw_ = g; }
  void set_l1_p7_requests_gateway(fapi::p7_requests_gateway* g) { l1_p7_gw_ = g; }
  void set_l1_p7_last_request_notifier(fapi::p7_last_request_notifier* n) { l1_p7_last_req_ = n; }

private:
  void receive_loop();
  void dispatch_message(uint16_t msg_type, void* data, uint32_t size);

  xsm_context       xsm_;
  bool              is_l2_ = false;
  std::atomic<bool> shutdown_done_{false};

  std::thread       receiver_thread_;
  std::atomic<bool> receiver_running_{false};

  int rx_cpu_id_   = -1;
  int rx_priority_ = 80;

  fapi::p7_indications_notifier*      l2_p7_ind_notifier_  = nullptr;
  fapi::p7_slot_indication_notifier*  l2_slot_notifier_    = nullptr;
  fapi::p5_responses_notifier*        l2_p5_resp_notifier_ = nullptr;
  fapi::error_indication_notifier*    l2_error_notifier_   = nullptr;

  fapi::p5_requests_gateway*      l1_p5_gw_       = nullptr;
  fapi::p7_requests_gateway*      l1_p7_gw_       = nullptr;
  fapi::p7_last_request_notifier* l1_p7_last_req_ = nullptr;

  static constexpr size_t TB_RING_SIZE = 8;
  std::array<std::vector<std::vector<uint8_t>>, TB_RING_SIZE> tb_ring_{};
  uint64_t                                                    tb_ring_idx_ = 0;

  std::vector<std::vector<uint8_t>> rx_tb_storages_;
};

} // namespace ocudu
