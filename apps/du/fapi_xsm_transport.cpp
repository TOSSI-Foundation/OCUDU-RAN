#include "fapi_xsm_transport.h"
#include "fapi_stats_record.h"
#include "fapi_xsm_logger.h"

#include "ocudu/ocudulog/ocudulog.h"
#include "ocudu/support/fapi_split_trace.h"

#include <chrono>
#include <cstdio>
#include <cstring>
#include <pthread.h>
#include <sched.h>

namespace ocudu {
bool fapi_xsm_verbose_logging = false;
} // namespace ocudu

using namespace ocudu;
using namespace ocudu::fapi_serial;

fapi_xsm_transport::~fapi_xsm_transport()
{
  shutdown();
}

void fapi_xsm_transport::init_l2_side()
{
  is_l2_ = true;
}

void fapi_xsm_transport::init_l1_side()
{
  is_l2_ = false;
}

void fapi_xsm_transport::shutdown()
{
  if (shutdown_done_.exchange(true)) {
    return;
  }
  stop_receiver();
  xsm_.close();
}

void fapi_xsm_transport::start_receiver()
{
  if (receiver_running_.load()) {
    return;
  }
  receiver_running_.store(true);
  receiver_thread_ = std::thread([this]() { receive_loop(); });

  sched_param param{};
  param.sched_priority = rx_priority_;
  pthread_setschedparam(receiver_thread_.native_handle(), SCHED_FIFO, &param);

  if (rx_cpu_id_ >= 0) {
    cpu_set_t cpuset;
    CPU_ZERO(&cpuset);
    CPU_SET(static_cast<unsigned>(rx_cpu_id_), &cpuset);
    pthread_setaffinity_np(receiver_thread_.native_handle(), sizeof(cpu_set_t), &cpuset);
  }
}

void fapi_xsm_transport::stop_receiver()
{
  if (!receiver_running_.load()) {
    return;
  }
  receiver_running_.store(false);
  xsm_.wake_up();
  if (receiver_thread_.joinable()) {
    receiver_thread_.join();
  }
}

void fapi_xsm_transport::receive_loop()
{
  using clock = std::chrono::steady_clock;
  constexpr auto stall_threshold = std::chrono::milliseconds(10);
  auto           t_last_iter_end = clock::now();
  uint64_t       rx_msgs_dispatched = 0;

  while (receiver_running_.load()) {
    auto t_iter_start = clock::now();
    auto loop_gap_dur = t_iter_start - t_last_iter_end;

    int  num_msgs    = xsm_.wait();
    auto t_after_wait = clock::now();
    auto wait_dur    = t_after_wait - t_iter_start;

    if (num_msgs <= 0 || !receiver_running_.load()) {
      t_last_iter_end = clock::now();
      continue;
    }

    int initial_num_msgs = num_msgs;

    while (num_msgs > 0) {
      uint32_t msg_size;
      uint16_t msg_type;
      uint16_t flags;
      uint64_t payload_pa = 0;

      void* va_msg = xsm_.get(msg_size, msg_type, flags, payload_pa);
      if (va_msg == nullptr) {
        break;
      }
      ++rx_msgs_dispatched;

      try {
        dispatch_message(msg_type, va_msg, msg_size);
      } catch (...) {

      }

      xsm_.return_pa(payload_pa);

      --num_msgs;
    }

    auto t_iter_end   = clock::now();
    auto dispatch_dur = t_iter_end - t_after_wait;
    if (wait_dur >= stall_threshold || dispatch_dur >= stall_threshold || loop_gap_dur >= stall_threshold) {
      static auto& stall_log   = ocudulog::fetch_basic_logger("FAPI");
      auto         wait_us     = std::chrono::duration_cast<std::chrono::microseconds>(wait_dur).count();
      auto         dispatch_us = std::chrono::duration_cast<std::chrono::microseconds>(dispatch_dur).count();
      auto         loop_gap_us = std::chrono::duration_cast<std::chrono::microseconds>(loop_gap_dur).count();
      stall_log.warning("[XSM-RX-STALL] wait_us={} dispatch_us={} loop_gap_us={} msgs={} dispatched={}",
                        wait_us,
                        dispatch_us,
                        loop_gap_us,
                        initial_num_msgs,
                        rx_msgs_dispatched);
    }
    t_last_iter_end = t_iter_end;
  }
}

void fapi_xsm_transport::dispatch_message(uint16_t msg_type, void* data, uint32_t size)
{
  static thread_local std::vector<uint8_t> local_msg_copy;
  if (local_msg_copy.size() < size) {
    local_msg_copy.resize(size);
  }
  std::memcpy(local_msg_copy.data(), data, size);

  auto* hdr = reinterpret_cast<fapi_xsm_msg_header*>(local_msg_copy.data());

  uint32_t header_size = static_cast<uint32_t>(sizeof(fapi_xsm_msg_header));
  if (size < header_size) {
    return;
  }
  uint32_t max_payload = size - header_size;
  uint32_t payload_len = hdr->msg_len;
  if (payload_len > max_payload) {
    payload_len = max_payload;
  }
  auto* payload = static_cast<const uint8_t*>(hdr->payload());

  buffer_reader r(payload, payload_len);

  uint64_t ipc_latency_ns = 0;
  if (hdr->time_stamp != 0) {
    const uint64_t now_ns = static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::steady_clock::now().time_since_epoch())
            .count());
    if (now_ns > hdr->time_stamp) {
      ipc_latency_ns = now_ns - hdr->time_stamp;
    }
  }

  const fapi_xsm_dir rx_dir_ind = fapi_xsm_dir::l1_to_l2_rx;
  const fapi_xsm_dir rx_dir_req = fapi_xsm_dir::l2_to_l1_rx;

  switch (msg_type) {
    case fapi_msg_type::SLOT_INDICATION: {
      if (payload_len >= 5) {
        const uint8_t* p     = static_cast<const uint8_t*>(hdr->payload());
        uint8_t        numer = p[0];
        uint32_t       count = static_cast<uint32_t>(p[1])        |
                               (static_cast<uint32_t>(p[2]) <<  8) |
                               (static_cast<uint32_t>(p[3]) << 16) |
                               (static_cast<uint32_t>(p[4]) << 24);
        if (numer > 4) {
          break;
        }
        const uint32_t max_count = 1024u * 1024u * 10u * (1u << numer);
        if (count >= max_count) {
          break;
        }

        static std::atomic<uint32_t> last_dispatched_slot_count{0xFFFFFFFFu};
        const uint32_t prev = last_dispatched_slot_count.load(std::memory_order_relaxed);
        if (count == prev) {
          break;
        }
        last_dispatched_slot_count.store(count, std::memory_order_relaxed);
      }
      fapi::slot_indication ind;
      deserialize(r, ind);
      if (fapi_stats::is_enabled()) {
        fapi_stats::record_fapi("RX_L1_L2", ind, static_cast<int>(size), ipc_latency_ns);
      }
      if (l2_slot_notifier_ != nullptr) {
        xsm_.rotate_dl_slots();
        l2_slot_notifier_->on_slot_indication(ind);
      }
      break;
    }
    case fapi_msg_type::RX_DATA_INDICATION: {
      fapi::rx_data_indication ind;
      rx_tb_storages_.clear();
      deserialize(r, ind, rx_tb_storages_);
      log_fapi(rx_dir_ind, ind, size);
      log_fapi_verbose(ind);
      if (fapi_stats::is_enabled()) {
        fapi_stats::record_fapi("RX_L1_L2", ind, static_cast<int>(size), ipc_latency_ns);
      }
      if (l2_p7_ind_notifier_ != nullptr) {
        l2_p7_ind_notifier_->on_rx_data_indication(ind);
      }
      break;
    }
    case fapi_msg_type::CRC_INDICATION: {
      fapi::crc_indication ind;
      deserialize(r, ind);
      log_fapi(rx_dir_ind, ind, size);
      if (fapi_stats::is_enabled()) {
        fapi_stats::record_fapi("RX_L1_L2", ind, static_cast<int>(size), ipc_latency_ns);
      }
      if (l2_p7_ind_notifier_ != nullptr) {
        l2_p7_ind_notifier_->on_crc_indication(ind);
      }
      break;
    }
    case fapi_msg_type::UCI_INDICATION: {
      fapi::uci_indication ind;
      deserialize(r, ind);
      log_fapi(rx_dir_ind, ind, size);
      if (fapi_stats::is_enabled()) {
        fapi_stats::record_fapi("RX_L1_L2", ind, static_cast<int>(size), ipc_latency_ns);
      }
      if (l2_p7_ind_notifier_ != nullptr) {
        l2_p7_ind_notifier_->on_uci_indication(ind);
      }
      break;
    }
    case fapi_msg_type::SRS_INDICATION: {
      fapi::srs_indication ind;
      deserialize(r, ind);
      log_fapi(rx_dir_ind, ind, size);
      if (fapi_stats::is_enabled()) {
        fapi_stats::record_fapi("RX_L1_L2", ind, static_cast<int>(size), ipc_latency_ns);
      }
      if (l2_p7_ind_notifier_ != nullptr) {
        l2_p7_ind_notifier_->on_srs_indication(ind);
      }
      break;
    }
    case fapi_msg_type::RACH_INDICATION: {
      fapi::rach_indication ind;
      deserialize(r, ind);
      log_fapi(rx_dir_ind, ind, size);
      if (fapi_stats::is_enabled()) {
        fapi_stats::record_fapi("RX_L1_L2", ind, static_cast<int>(size), ipc_latency_ns);
      }
      if (l2_p7_ind_notifier_ != nullptr) {
        l2_p7_ind_notifier_->on_rach_indication(ind);
      }
      break;
    }
    case fapi_msg_type::PARAM_RESPONSE: {
      fapi::param_response resp;
      deserialize(r, resp);
      log_fapi(rx_dir_ind, resp, size);
      log_fapi_verbose(resp);
      if (fapi_stats::is_enabled()) {
        fapi_stats::record_fapi("RX_L1_L2", resp, static_cast<int>(size), ipc_latency_ns);
      }
      if (l2_p5_resp_notifier_ != nullptr) {
        l2_p5_resp_notifier_->on_param_response(resp);
      }
      break;
    }
    case fapi_msg_type::CONFIG_RESPONSE: {
      fapi::config_response resp;
      deserialize(r, resp);
      log_fapi(rx_dir_ind, resp, size);
      log_fapi_verbose(resp);
      if (fapi_stats::is_enabled()) {
        fapi_stats::record_fapi("RX_L1_L2", resp, static_cast<int>(size), ipc_latency_ns);
      }
      if (l2_p5_resp_notifier_ != nullptr) {
        l2_p5_resp_notifier_->on_config_response(resp);
      }
      break;
    }
    case fapi_msg_type::STOP_INDICATION: {
      fapi::stop_indication ind;
      deserialize(r, ind);
      log_fapi(rx_dir_ind, ind, size);
      log_fapi_verbose(ind);
      if (fapi_stats::is_enabled()) {
        fapi_stats::record_fapi("RX_L1_L2", ind, static_cast<int>(size), ipc_latency_ns);
      }
      if (l2_p5_resp_notifier_ != nullptr) {
        l2_p5_resp_notifier_->on_stop_indication(ind);
      }
      break;
    }
    case fapi_msg_type::ERROR_INDICATION: {
      fapi::error_indication ind;
      deserialize(r, ind);
      log_fapi(rx_dir_ind, ind, size);
      if (fapi_stats::is_enabled()) {
        fapi_stats::record_fapi("RX_L1_L2", ind, static_cast<int>(size), ipc_latency_ns);
      }
      if (l2_error_notifier_ != nullptr) {
        l2_error_notifier_->on_error_indication(ind);
      }
      break;
    }

    case fapi_msg_type::PARAM_REQUEST: {
      fapi::param_request req;
      deserialize(r, req);
      log_fapi(rx_dir_req, req, size);
      log_fapi_verbose(req);
      if (fapi_stats::is_enabled()) {
        fapi_stats::record_fapi("RX_L2_L1", req, static_cast<int>(size), ipc_latency_ns);
      }
      if (l1_p5_gw_ != nullptr) {
        l1_p5_gw_->send_param_request(req);
      }
      break;
    }
    case fapi_msg_type::CONFIG_REQUEST: {
      fapi::config_request req;
      deserialize(r, req);
      log_fapi(rx_dir_req, req, size);
      log_fapi_verbose(req);
      if (fapi_stats::is_enabled()) {
        fapi_stats::record_fapi("RX_L2_L1", req, static_cast<int>(size), ipc_latency_ns);
      }
      if (l1_p5_gw_ != nullptr) {
        l1_p5_gw_->send_config_request(req);
      }
      break;
    }
    case fapi_msg_type::START_REQUEST: {
      fapi::start_request req;
      deserialize(r, req);
      log_fapi(rx_dir_req, req, size);
      log_fapi_verbose(req);
      if (fapi_stats::is_enabled()) {
        fapi_stats::record_fapi("RX_L2_L1", req, static_cast<int>(size), ipc_latency_ns);
      }
      if (l1_p5_gw_ != nullptr) {
        l1_p5_gw_->send_start_request(req);
      }
      break;
    }
    case fapi_msg_type::STOP_REQUEST: {
      fapi::stop_request req;
      deserialize(r, req);
      log_fapi(rx_dir_req, req, size);
      log_fapi_verbose(req);
      if (fapi_stats::is_enabled()) {
        fapi_stats::record_fapi("RX_L2_L1", req, static_cast<int>(size), ipc_latency_ns);
      }
      if (l1_p5_gw_ != nullptr) {
        l1_p5_gw_->send_stop_request(req);
      }
      break;
    }
    case fapi_msg_type::DL_TTI_REQUEST: {
      fapi::dl_tti_request req;
      deserialize(r, req);
      log_fapi(rx_dir_req, req, size);
      log_fapi_verbose(req);
      if (fapi_stats::is_enabled()) {
        fapi_stats::record_fapi("RX_L2_L1", req, static_cast<int>(size), ipc_latency_ns);
      }
      if (l1_p7_gw_ != nullptr) {
        l1_p7_gw_->send_dl_tti_request(req);
      }
      break;
    }
    case fapi_msg_type::UL_TTI_REQUEST: {
      fapi::ul_tti_request req;
      deserialize(r, req);
      log_fapi(rx_dir_req, req, size);
      if (fapi_stats::is_enabled()) {
        fapi_stats::record_fapi("RX_L2_L1", req, static_cast<int>(size), ipc_latency_ns);
      }
      if (l1_p7_gw_ != nullptr) {
        l1_p7_gw_->send_ul_tti_request(req);
      }
      break;
    }
    case fapi_msg_type::UL_DCI_REQUEST: {
      fapi::ul_dci_request req;
      deserialize(r, req);
      log_fapi(rx_dir_req, req, size);
      if (fapi_stats::is_enabled()) {
        fapi_stats::record_fapi("RX_L2_L1", req, static_cast<int>(size), ipc_latency_ns);
      }
      if (l1_p7_gw_ != nullptr) {
        l1_p7_gw_->send_ul_dci_request(req);
      }
      break;
    }
    case fapi_msg_type::TX_DATA_REQUEST: {
      fapi::tx_data_request req;
      auto& tb_storages = tb_ring_[tb_ring_idx_ % TB_RING_SIZE];
      ++tb_ring_idx_;
      tb_storages.clear();
      deserialize(r, req, tb_storages);
      log_fapi(rx_dir_req, req, size);
      if (fapi_stats::is_enabled()) {
        fapi_stats::record_fapi("RX_L2_L1", req, static_cast<int>(size), ipc_latency_ns);
      }
      if (l1_p7_gw_ != nullptr) {
        l1_p7_gw_->send_tx_data_request(req);
      }
      break;
    }
    case fapi_msg_type::VENDOR_MSG_HEADER_IND: {
      break;
    }

    case 0x8F: {
      slot_point slot;
      fapi_serial::deserialize(r, slot);
      if (fapi_xsm_verbose_logging) {
        std::printf("[FAPI-XSM %lu] [RX L2->L1] p7_last_request slot=%u.%u size=%uB\n",
                    fapi_xsm_log_timestamp_ns(), slot.sfn(), slot.slot_index(), size);
      }
      if (fapi_stats::is_enabled()) {
        fapi_stats::record_fapi_last_message("RX_L2_L1", slot, static_cast<int>(size), ipc_latency_ns);
      }
      if (l1_p7_last_req_ != nullptr) {
        l1_p7_last_req_->on_last_message(slot);
      }
      break;
    }

    default:
      break;
  }
}
