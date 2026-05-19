#pragma once

#include "lib/fapi/serialization/fapi_message_type_id.h"
#include "ocudu/fapi/common/error_indication.h"
#include "ocudu/fapi/p5/p5_messages.h"
#include "ocudu/fapi/p7/messages/crc_indication.h"
#include "ocudu/fapi/p7/messages/dl_tti_request.h"
#include "ocudu/fapi/p7/messages/rach_indication.h"
#include "ocudu/fapi/p7/messages/rx_data_indication.h"
#include "ocudu/fapi/p7/messages/slot_indication.h"
#include "ocudu/fapi/p7/messages/srs_indication.h"
#include "ocudu/fapi/p7/messages/tx_data_request.h"
#include "ocudu/fapi/p7/messages/uci_indication.h"
#include "ocudu/fapi/p7/messages/ul_dci_request.h"
#include "ocudu/fapi/p7/messages/ul_tti_request.h"
#include "ocudu/support/fapi_split_trace.h"
#include <chrono>
#include <cstdio>

namespace ocudu {

extern bool fapi_xsm_verbose_logging;

enum class fapi_xsm_dir : uint8_t {
  l2_to_l1_tx,
  l2_to_l1_rx,
  l1_to_l2_tx,
  l1_to_l2_rx
};

inline const char* fapi_xsm_dir_str(fapi_xsm_dir d)
{
  switch (d) {
    case fapi_xsm_dir::l2_to_l1_tx: return "TX L2->L1";
    case fapi_xsm_dir::l2_to_l1_rx: return "RX L2->L1";
    case fapi_xsm_dir::l1_to_l2_tx: return "TX L1->L2";
    case fapi_xsm_dir::l1_to_l2_rx: return "RX L1->L2";
  }
  return "?";
}

inline uint64_t fapi_xsm_log_timestamp_ns()
{
  auto now = std::chrono::steady_clock::now();
  return static_cast<uint64_t>(
      std::chrono::duration_cast<std::chrono::nanoseconds>(now.time_since_epoch()).count());
}

inline std::pair<const char*, const char*> fapi_xsm_endpoints(fapi_xsm_dir d)
{
  switch (d) {
    case fapi_xsm_dir::l2_to_l1_tx: return {"odu_high/MAC", "odu_low/PHY"};
    case fapi_xsm_dir::l2_to_l1_rx: return {"odu_high/MAC", "odu_low/PHY"};
    case fapi_xsm_dir::l1_to_l2_tx: return {"odu_low/PHY", "odu_high/MAC"};
    case fapi_xsm_dir::l1_to_l2_rx: return {"odu_low/PHY", "odu_high/MAC"};
  }
  return {"?", "?"};
}

inline void fapi_xsm_log_concise(fapi_xsm_dir /*d*/, uint8_t /*msg_type*/, uint32_t /*size*/, const char* /*details*/)
{
}


inline void log_fapi(fapi_xsm_dir d, const fapi::param_request&, uint32_t sz)
{
  fapi_xsm_log_concise(d, fapi_serial::fapi_msg_type::PARAM_REQUEST, sz, "");
}

inline void log_fapi(fapi_xsm_dir d, const fapi::param_response& m, uint32_t sz)
{
  char buf[64];
  std::snprintf(buf, sizeof(buf), "error_code=%u", static_cast<unsigned>(m.error_code));
  fapi_xsm_log_concise(d, fapi_serial::fapi_msg_type::PARAM_RESPONSE, sz, buf);
}

inline void log_fapi(fapi_xsm_dir d, const fapi::config_request& m, uint32_t sz)
{
  char buf[128];
  std::snprintf(buf, sizeof(buf), "pci=%u scs=%u duplex=%u",
                static_cast<unsigned>(m.cell_cfg.pci),
                static_cast<unsigned>(m.cell_cfg.scs_common),
                static_cast<unsigned>(m.cell_cfg.duplex));
  fapi_xsm_log_concise(d, fapi_serial::fapi_msg_type::CONFIG_REQUEST, sz, buf);
}

inline void log_fapi(fapi_xsm_dir d, const fapi::config_response& m, uint32_t sz)
{
  char buf[64];
  std::snprintf(buf, sizeof(buf), "error_code=%u", static_cast<unsigned>(m.error_code));
  fapi_xsm_log_concise(d, fapi_serial::fapi_msg_type::CONFIG_RESPONSE, sz, buf);
}

inline void log_fapi(fapi_xsm_dir d, const fapi::start_request&, uint32_t sz)
{
  fapi_xsm_log_concise(d, fapi_serial::fapi_msg_type::START_REQUEST, sz, "");
}

inline void log_fapi(fapi_xsm_dir d, const fapi::stop_request&, uint32_t sz)
{
  fapi_xsm_log_concise(d, fapi_serial::fapi_msg_type::STOP_REQUEST, sz, "");
}

inline void log_fapi(fapi_xsm_dir d, const fapi::stop_indication&, uint32_t sz)
{
  fapi_xsm_log_concise(d, fapi_serial::fapi_msg_type::STOP_INDICATION, sz, "");
}

inline void log_fapi(fapi_xsm_dir d, const fapi::slot_indication& m, uint32_t sz)
{
  char buf[96];
  std::snprintf(buf, sizeof(buf), "slot_sfn_slot=%u.%u",
                m.slot.sfn(), m.slot.slot_index());
  fapi_xsm_log_concise(d, fapi_serial::fapi_msg_type::SLOT_INDICATION, sz, buf);
}

inline void log_fapi(fapi_xsm_dir d, const fapi::dl_tti_request& m, uint32_t sz)
{
  char buf[128];
  std::snprintf(buf, sizeof(buf), "slot=%u.%u pdus=%zu",
                m.slot.sfn(), m.slot.slot_index(), m.pdus.size());
  fapi_xsm_log_concise(d, fapi_serial::fapi_msg_type::DL_TTI_REQUEST, sz, buf);
}

inline void log_fapi(fapi_xsm_dir d, const fapi::ul_tti_request& m, uint32_t sz)
{
  char buf[128];
  std::snprintf(buf, sizeof(buf), "slot=%u.%u pdus=%zu groups=%u",
                m.slot.sfn(), m.slot.slot_index(), m.pdus.size(), m.num_groups);
  fapi_xsm_log_concise(d, fapi_serial::fapi_msg_type::UL_TTI_REQUEST, sz, buf);
}

inline void log_fapi(fapi_xsm_dir d, const fapi::ul_dci_request& m, uint32_t sz)
{
  char buf[96];
  std::snprintf(buf, sizeof(buf), "slot=%u.%u pdus=%zu",
                m.slot.sfn(), m.slot.slot_index(), m.pdus.size());
  fapi_xsm_log_concise(d, fapi_serial::fapi_msg_type::UL_DCI_REQUEST, sz, buf);
}

inline void log_fapi(fapi_xsm_dir d, const fapi::tx_data_request& m, uint32_t sz)
{
  char buf[96];
  std::snprintf(buf, sizeof(buf), "slot=%u.%u pdus=%zu",
                m.slot.sfn(), m.slot.slot_index(), m.pdus.size());
  fapi_xsm_log_concise(d, fapi_serial::fapi_msg_type::TX_DATA_REQUEST, sz, buf);
}

inline void log_fapi(fapi_xsm_dir d, const fapi::rx_data_indication& m, uint32_t sz)
{
  char buf[96];
  std::snprintf(buf, sizeof(buf), "slot=%u.%u pdus=%zu",
                m.slot.sfn(), m.slot.slot_index(), m.pdus.size());
  fapi_xsm_log_concise(d, fapi_serial::fapi_msg_type::RX_DATA_INDICATION, sz, buf);
}

inline void log_fapi(fapi_xsm_dir d, const fapi::crc_indication& m, uint32_t sz)
{
  char buf[96];
  std::snprintf(buf, sizeof(buf), "slot=%u.%u pdus=%zu",
                m.slot.sfn(), m.slot.slot_index(), m.pdus.size());
  fapi_xsm_log_concise(d, fapi_serial::fapi_msg_type::CRC_INDICATION, sz, buf);
}

inline void log_fapi(fapi_xsm_dir d, const fapi::uci_indication& m, uint32_t sz)
{
  char buf[96];
  std::snprintf(buf, sizeof(buf), "slot=%u.%u pdus=%zu",
                m.slot.sfn(), m.slot.slot_index(), m.pdus.size());
  fapi_xsm_log_concise(d, fapi_serial::fapi_msg_type::UCI_INDICATION, sz, buf);
}

inline void log_fapi(fapi_xsm_dir d, const fapi::srs_indication& m, uint32_t sz)
{
  char buf[96];
  std::snprintf(buf, sizeof(buf), "slot=%u.%u pdus=%zu",
                m.slot.sfn(), m.slot.slot_index(), m.pdus.size());
  fapi_xsm_log_concise(d, fapi_serial::fapi_msg_type::SRS_INDICATION, sz, buf);
}

inline void log_fapi(fapi_xsm_dir d, const fapi::rach_indication& m, uint32_t sz)
{
  char buf[96];
  std::snprintf(buf, sizeof(buf), "slot=%u.%u pdus=%zu",
                m.slot.sfn(), m.slot.slot_index(), m.pdus.size());
  fapi_xsm_log_concise(d, fapi_serial::fapi_msg_type::RACH_INDICATION, sz, buf);
}

inline void log_fapi(fapi_xsm_dir d, const fapi::error_indication& m, uint32_t sz)
{
  char buf[128];
  std::snprintf(buf, sizeof(buf), "msg_id=0x%02x error_code=%u",
                static_cast<unsigned>(m.message_id),
                static_cast<unsigned>(m.error_code));
  fapi_xsm_log_concise(d, fapi_serial::fapi_msg_type::ERROR_INDICATION, sz, buf);
}

#ifndef XSM_VERBOSE_PRINTF
#define XSM_VERBOSE_PRINTF(...) ((void)0)
#endif

inline void log_fapi_verbose(const fapi::param_request&)
{
  XSM_VERBOSE_PRINTF("[FAPI-XSM P5] param_request: (no payload fields per SCF 222.10)\n");
}

inline void log_fapi_verbose(const fapi::config_request& m)
{
  XSM_VERBOSE_PRINTF("[FAPI-XSM P5] config_request:\n"
              "  scs_common=%u  cp=%u  pci=%u  duplex=%u\n"
              "  carrier: dl_bw=%u ul_bw=%u dl_arfcn=%u ul_arfcn=%u tx_ant=%u rx_ant=%u\n"
              "  prach: prach_cfg_idx=%u msg1_fdm=%u msg1_freq_start=%u zcz=%u\n"
              "  ssb: period=%u block_power=%d\n",
              static_cast<unsigned>(m.cell_cfg.scs_common),
              static_cast<unsigned>(m.cell_cfg.cp.value),
              static_cast<unsigned>(m.cell_cfg.pci),
              static_cast<unsigned>(m.cell_cfg.duplex),
              m.cell_cfg.carrier_cfg.dl_bandwidth,
              m.cell_cfg.carrier_cfg.ul_bandwidth,
              m.cell_cfg.carrier_cfg.dl_f_ref_arfcn,
              m.cell_cfg.carrier_cfg.ul_f_ref_arfcn,
              m.cell_cfg.carrier_cfg.num_tx_ant,
              m.cell_cfg.carrier_cfg.num_rx_ant,
              static_cast<unsigned>(m.cell_cfg.prach_cfg.rach_cfg_generic.prach_config_index),
              m.cell_cfg.prach_cfg.rach_cfg_generic.msg1_fdm,
              m.cell_cfg.prach_cfg.rach_cfg_generic.msg1_frequency_start,
              static_cast<unsigned>(m.cell_cfg.prach_cfg.rach_cfg_generic.zero_correlation_zone_config),
              static_cast<unsigned>(m.cell_cfg.ssb_cfg.ssb_period),
              m.cell_cfg.ssb_cfg.ssb_block_power);
}

inline void log_fapi_verbose(const fapi::param_response& m)
{
  XSM_VERBOSE_PRINTF("[FAPI-XSM P5] param_response: error_code=0x%02x (%s)\n",
              static_cast<unsigned>(m.error_code),
              m.error_code == fapi::error_code_id::msg_ok ? "MSG_OK" : "ERROR");
}

inline void log_fapi_verbose(const fapi::config_response& m)
{
  XSM_VERBOSE_PRINTF("[FAPI-XSM P5] config_response: error_code=0x%02x (%s)\n",
              static_cast<unsigned>(m.error_code),
              m.error_code == fapi::error_code_id::msg_ok ? "MSG_OK" : "ERROR");
}

inline void log_fapi_verbose(const fapi::start_request&)
{
  XSM_VERBOSE_PRINTF("[FAPI-XSM P5] start_request: (no payload fields per SCF 222.10)\n");
}

inline void log_fapi_verbose(const fapi::stop_request&)
{
  XSM_VERBOSE_PRINTF("[FAPI-XSM P5] stop_request: (no payload fields per SCF 222.10)\n");
}

inline void log_fapi_verbose(const fapi::stop_indication&)
{
  XSM_VERBOSE_PRINTF("[FAPI-XSM P5] stop_indication: (no payload fields per SCF 222.10)\n");
}

inline void log_fapi_verbose(const fapi::slot_indication& m)
{
  if (!fapi_xsm_verbose_logging) return;
  XSM_VERBOSE_PRINTF("[FAPI-XSM verbose] slot_indication: sfn=%u slot=%u numerology=%u\n",
              m.slot.sfn(), m.slot.slot_index(), m.slot.numerology());
}

inline void log_fapi_verbose(const fapi::dl_tti_request& m)
{
  if (!fapi_xsm_verbose_logging) return;
  XSM_VERBOSE_PRINTF("[FAPI-XSM verbose] dl_tti_request: sfn=%u slot=%u nof_pdus=%zu\n",
              m.slot.sfn(), m.slot.slot_index(), m.pdus.size());
  for (size_t i = 0; i < m.pdus.size(); ++i) {
    XSM_VERBOSE_PRINTF("  pdu[%zu]: type=%u\n", i, static_cast<unsigned>(m.pdus[i].pdu_type));
  }
}

inline void log_fapi_verbose(const fapi::ul_tti_request& m)
{
  if (!fapi_xsm_verbose_logging) return;
  XSM_VERBOSE_PRINTF("[FAPI-XSM verbose] ul_tti_request: sfn=%u slot=%u pdus=%zu num_groups=%u\n",
              m.slot.sfn(), m.slot.slot_index(), m.pdus.size(), m.num_groups);
}

inline void log_fapi_verbose(const fapi::rx_data_indication& m)
{
  if (!fapi_xsm_verbose_logging) return;
  XSM_VERBOSE_PRINTF("[FAPI-XSM verbose] rx_data_indication: sfn=%u slot=%u pdus=%zu\n",
              m.slot.sfn(), m.slot.slot_index(), m.pdus.size());
  for (size_t i = 0; i < m.pdus.size(); ++i) {
    XSM_VERBOSE_PRINTF("  pdu[%zu]: rnti=0x%04x harq=%u tb_size=%zuB\n", i,
                static_cast<unsigned>(m.pdus[i].rnti),
                static_cast<unsigned>(m.pdus[i].harq_id),
                m.pdus[i].transport_block.size());
  }
}

template <typename T>
inline void log_fapi_verbose(const T&)
{
}

} // namespace ocudu
