#pragma once

#include "fapi_stats_record.h"
#include "fapi_xsm_logger.h"
#include "ocudu/support/fapi_split_trace.h"
#include "lib/fapi/serialization/fapi_message_type_id.h"
#include "lib/fapi/serialization/fapi_serialization_buffer.h"
#include "lib/fapi/serialization/fapi_serializer_p5.h"
#include "lib/fapi/serialization/fapi_serializer_p7_dl.h"
#include "lib/fapi/serialization/fapi_serializer_p7_ind.h"
#include "lib/fapi/serialization/fapi_serializer_p7_ul.h"
#include "lib/fapi/serialization/fapi_xsm_message_header.h"
#include "lib/ipc/xsm/xsm_context.h"
#include "ocudu/fapi/common/error_indication_notifier.h"
#include "ocudu/fapi/p5/p5_requests_gateway.h"
#include "ocudu/fapi/p5/p5_responses_notifier.h"
#include "ocudu/fapi/p7/p7_indications_notifier.h"
#include "ocudu/fapi/p7/p7_last_request_notifier.h"
#include "ocudu/fapi/p7/p7_requests_gateway.h"
#include "ocudu/fapi/p7/p7_slot_indication_notifier.h"
#include "ocudu/ocudulog/logger.h"
#include "ocudu/ocudulog/ocudulog.h"
#include <chrono>
#include <cstring>
#include <string>

namespace ocudu {

template <typename MsgT>
inline void xsm_send_message(xsm_context& xsm, uint8_t msg_type_id, const MsgT& msg, bool track_dl = false)
{
  if (!xsm.is_peer_alive()) {
    return;
  }

  void* buf = xsm.alloc_buffer();
  if (buf == nullptr) {
    return;
  }

  auto* hdr = static_cast<fapi_serial::fapi_xsm_msg_header*>(buf);
  std::memset(hdr, 0, sizeof(*hdr));
  hdr->msg_type              = msg_type_id;
  hdr->num_messages_in_block = 1;

  void*    payload     = hdr->payload();
  uint32_t payload_cap = XSM_BLOCK_SIZE - static_cast<uint32_t>(sizeof(*hdr));

  fapi_serial::buffer_writer w(payload, payload_cap);
  fapi_serial::serialize(w, msg);
  hdr->msg_len = w.bytes_written();

  auto now        = std::chrono::steady_clock::now();
  hdr->time_stamp = static_cast<uint64_t>(
      std::chrono::duration_cast<std::chrono::nanoseconds>(now.time_since_epoch()).count());

  uint32_t total_size = static_cast<uint32_t>(sizeof(*hdr)) + w.bytes_written();

  const fapi_xsm_dir dir = track_dl ? fapi_xsm_dir::l2_to_l1_tx : fapi_xsm_dir::l1_to_l2_tx;
  if (msg_type_id != fapi_serial::fapi_msg_type::SLOT_INDICATION) {
    log_fapi(dir, msg, total_size);
    log_fapi_verbose(msg);
  }

  int ret = xsm.put(buf, total_size, msg_type_id, 0);
  if (ret != 0) {
    xsm.free_buffer(buf);
    return;
  }

  if (fapi_stats::is_enabled()) {
    fapi_stats::record_fapi(track_dl ? "TX_L2_L1" : "TX_L1_L2",
                            msg, static_cast<int>(total_size), 0);
  }

  if (track_dl) {
    xsm_buffer_desc* desc = xsm.find_buffer(buf);
    if (desc != nullptr) {
      xsm.add_dl_buffer_to_current_slot(desc);
    } else {
      xsm.free_buffer(buf);
    }
  } else {
    xsm_buffer_desc* desc = xsm.find_buffer(buf);
    if (desc != nullptr) {
      xsm.add_ul_buffer_to_current_slot(desc);
    } else {
      xsm.free_buffer(buf);
    }
  }
}

class xsm_p5_requests_gateway : public fapi::p5_requests_gateway
{
  xsm_context& xsm_;
public:
  explicit xsm_p5_requests_gateway(xsm_context& xsm) : xsm_(xsm) {}

  void send_param_request(const fapi::param_request& msg) override
  {
    xsm_send_message(xsm_, fapi_serial::fapi_msg_type::PARAM_REQUEST, msg, true);
  }
  void send_config_request(const fapi::config_request& msg) override
  {
    xsm_send_message(xsm_, fapi_serial::fapi_msg_type::CONFIG_REQUEST, msg, true);
  }
  void send_start_request(const fapi::start_request& msg) override
  {
    xsm_send_message(xsm_, fapi_serial::fapi_msg_type::START_REQUEST, msg, true);
  }
  void send_stop_request(const fapi::stop_request& msg) override
  {
    xsm_send_message(xsm_, fapi_serial::fapi_msg_type::STOP_REQUEST, msg, true);
  }
};

class xsm_p7_requests_gateway : public fapi::p7_requests_gateway
{
  xsm_context& xsm_;

public:
  explicit xsm_p7_requests_gateway(xsm_context& xsm) : xsm_(xsm) {}

  void send_dl_tti_request(const fapi::dl_tti_request& msg) override
  {
    xsm_send_message(xsm_, fapi_serial::fapi_msg_type::DL_TTI_REQUEST, msg, true);
  }
  void send_ul_tti_request(const fapi::ul_tti_request& msg) override
  {
    xsm_send_message(xsm_, fapi_serial::fapi_msg_type::UL_TTI_REQUEST, msg, true);
  }
  void send_ul_dci_request(const fapi::ul_dci_request& msg) override
  {
    xsm_send_message(xsm_, fapi_serial::fapi_msg_type::UL_DCI_REQUEST, msg, true);
  }
  void send_tx_data_request(const fapi::tx_data_request& msg) override
  {
    xsm_send_message(xsm_, fapi_serial::fapi_msg_type::TX_DATA_REQUEST, msg, true);
  }
};

class xsm_p7_last_request_notifier : public fapi::p7_last_request_notifier
{
  xsm_context& xsm_;
public:
  explicit xsm_p7_last_request_notifier(xsm_context& xsm) : xsm_(xsm) {}

  void on_last_message(slot_point slot) override
  {
    void* buf = xsm_.alloc_buffer();
    if (buf == nullptr) {
      return;
    }
    auto* hdr = static_cast<fapi_serial::fapi_xsm_msg_header*>(buf);
    std::memset(hdr, 0, sizeof(*hdr));
    hdr->msg_type              = 0x8F;
    hdr->num_messages_in_block = 1;

    fapi_serial::buffer_writer w(hdr->payload(), XSM_BLOCK_SIZE - sizeof(*hdr));
    fapi_serial::serialize(w, slot);
    hdr->msg_len = w.bytes_written();

    uint32_t total = sizeof(*hdr) + w.bytes_written();
    if (fapi_xsm_verbose_logging) {
      std::printf("[FAPI-XSM %lu] [TX L2->L1] p7_last_request slot=%u.%u size=%uB\n",
                  fapi_xsm_log_timestamp_ns(), slot.sfn(), slot.slot_index(), total);
    }

    int ret = xsm_.put(buf, total, 0x8F, 0);
    if (ret != 0) {
      xsm_.free_buffer(buf);
      return;
    }

    xsm_buffer_desc* desc = xsm_.find_buffer(buf);
    if (desc != nullptr) {
      xsm_.add_dl_buffer_to_current_slot(desc);
    } else {
      xsm_.free_buffer(buf);
    }

    if (fapi_stats::is_enabled()) {
      fapi_stats::record_fapi_last_message("TX_L2_L1", slot, static_cast<int>(total), 0);
    }
  }
};

class xsm_p5_responses_notifier : public fapi::p5_responses_notifier
{
  xsm_context& xsm_;
public:
  explicit xsm_p5_responses_notifier(xsm_context& xsm) : xsm_(xsm) {}

  void on_param_response(const fapi::param_response& msg) override
  {
    xsm_send_message(xsm_, fapi_serial::fapi_msg_type::PARAM_RESPONSE, msg);
  }
  void on_config_response(const fapi::config_response& msg) override
  {
    xsm_send_message(xsm_, fapi_serial::fapi_msg_type::CONFIG_RESPONSE, msg);
  }
  void on_stop_indication(const fapi::stop_indication& msg) override
  {
    xsm_send_message(xsm_, fapi_serial::fapi_msg_type::STOP_INDICATION, msg);
  }
};

class xsm_p7_indications_notifier : public fapi::p7_indications_notifier
{
  xsm_context& xsm_;
public:
  explicit xsm_p7_indications_notifier(xsm_context& xsm) : xsm_(xsm) {}

  void on_rx_data_indication(const fapi::rx_data_indication& msg) override
  {
    xsm_send_message(xsm_, fapi_serial::fapi_msg_type::RX_DATA_INDICATION, msg);
  }
  void on_crc_indication(const fapi::crc_indication& msg) override
  {
    xsm_send_message(xsm_, fapi_serial::fapi_msg_type::CRC_INDICATION, msg);
  }
  void on_uci_indication(const fapi::uci_indication& msg) override
  {
    xsm_send_message(xsm_, fapi_serial::fapi_msg_type::UCI_INDICATION, msg);
  }
  void on_srs_indication(const fapi::srs_indication& msg) override
  {
    xsm_send_message(xsm_, fapi_serial::fapi_msg_type::SRS_INDICATION, msg);
  }
  void on_rach_indication(const fapi::rach_indication& msg) override
  {
    xsm_send_message(xsm_, fapi_serial::fapi_msg_type::RACH_INDICATION, msg);
  }
};

class xsm_p7_slot_indication_notifier : public fapi::p7_slot_indication_notifier
{
  xsm_context& xsm_;
public:
  explicit xsm_p7_slot_indication_notifier(xsm_context& xsm) : xsm_(xsm) {}

  void on_slot_indication(const fapi::slot_indication& msg) override
  {
    xsm_.rotate_ul_slots();
    xsm_send_message(xsm_, fapi_serial::fapi_msg_type::SLOT_INDICATION, msg);
  }
};

class xsm_error_indication_notifier : public fapi::error_indication_notifier
{
  xsm_context& xsm_;
public:
  explicit xsm_error_indication_notifier(xsm_context& xsm) : xsm_(xsm) {}

  void on_error_indication(const fapi::error_indication& msg) override
  {
    xsm_send_message(xsm_, fapi_serial::fapi_msg_type::ERROR_INDICATION, msg);
  }
};

} // namespace ocudu
