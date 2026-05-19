#pragma once

#include "fapi_stats.h"
#include "fapi_stats_serializers.h"
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

#include <cstdint>

namespace ocudu::fapi_stats {

inline void record_fapi(const char* direction, const fapi::param_request& msg, int total_size, uint64_t lat_ns = 0)
{
  char content[MAX_MESSAGE_CONTENT_LEN];
  int  n = serialize_param_request(content, sizeof(content), msg);
  record("PARAM_REQUEST", direction, 0, 0, total_size, n, lat_ns, content);
}

inline void record_fapi(const char* direction, const fapi::param_response& msg, int total_size, uint64_t lat_ns = 0)
{
  char content[MAX_MESSAGE_CONTENT_LEN];
  int  n = serialize_param_response(content, sizeof(content), msg);
  record("PARAM_RESPONSE", direction, 0, 0, total_size, n, lat_ns, content);
}

inline void record_fapi(const char* direction, const fapi::config_request& msg, int total_size, uint64_t lat_ns = 0)
{
  char content[MAX_MESSAGE_CONTENT_LEN];
  int  n = serialize_config_request(content, sizeof(content), msg);
  record("CONFIG_REQUEST", direction, 0, 0, total_size, n, lat_ns, content);
}

inline void record_fapi(const char* direction, const fapi::config_response& msg, int total_size, uint64_t lat_ns = 0)
{
  char content[MAX_MESSAGE_CONTENT_LEN];
  int  n = serialize_config_response(content, sizeof(content), msg);
  record("CONFIG_RESPONSE", direction, 0, 0, total_size, n, lat_ns, content);
}

inline void record_fapi(const char* direction, const fapi::start_request& msg, int total_size, uint64_t lat_ns = 0)
{
  char content[MAX_MESSAGE_CONTENT_LEN];
  int  n = serialize_start_request(content, sizeof(content), msg);
  record("START_REQUEST", direction, 0, 0, total_size, n, lat_ns, content);
}

inline void record_fapi(const char* direction, const fapi::stop_request& msg, int total_size, uint64_t lat_ns = 0)
{
  char content[MAX_MESSAGE_CONTENT_LEN];
  int  n = serialize_stop_request(content, sizeof(content), msg);
  record("STOP_REQUEST", direction, 0, 0, total_size, n, lat_ns, content);
}

inline void record_fapi(const char* direction, const fapi::stop_indication& msg, int total_size, uint64_t lat_ns = 0)
{
  char content[MAX_MESSAGE_CONTENT_LEN];
  int  n = serialize_stop_indication(content, sizeof(content), msg);
  record("STOP_INDICATION", direction, 0, 0, total_size, n, lat_ns, content);
}

inline void record_fapi(const char* direction, const fapi::error_indication& msg, int total_size, uint64_t lat_ns = 0)
{
  char content[MAX_MESSAGE_CONTENT_LEN];
  int  n   = serialize_error_indication(content, sizeof(content), msg);
  int  sfn = msg.slot.has_value() ? static_cast<int>(msg.slot->sfn()) : 0;
  int  sl  = msg.slot.has_value() ? static_cast<int>(msg.slot->slot_index()) : 0;
  record("ERROR_INDICATION", direction, sfn, sl, total_size, n, lat_ns, content);
}


inline void record_fapi(const char* direction, const fapi::dl_tti_request& msg, int total_size, uint64_t lat_ns = 0)
{
  char content[MAX_MESSAGE_CONTENT_LEN];
  int  n = serialize_dl_tti_request(content, sizeof(content), msg);
  record("DL_TTI_REQUEST", direction, msg.slot.sfn(), msg.slot.slot_index(), total_size, n, lat_ns, content);
}

inline void record_fapi(const char* direction, const fapi::ul_tti_request& msg, int total_size, uint64_t lat_ns = 0)
{
  char content[MAX_MESSAGE_CONTENT_LEN];
  int  n = serialize_ul_tti_request(content, sizeof(content), msg);
  record("UL_TTI_REQUEST", direction, msg.slot.sfn(), msg.slot.slot_index(), total_size, n, lat_ns, content);
}

inline void record_fapi(const char* direction, const fapi::ul_dci_request& msg, int total_size, uint64_t lat_ns = 0)
{
  char content[MAX_MESSAGE_CONTENT_LEN];
  int  n = serialize_ul_dci_request(content, sizeof(content), msg);
  record("UL_DCI_REQUEST", direction, msg.slot.sfn(), msg.slot.slot_index(), total_size, n, lat_ns, content);
}

inline void record_fapi(const char* direction, const fapi::tx_data_request& msg, int total_size, uint64_t lat_ns = 0)
{
  char content[MAX_MESSAGE_CONTENT_LEN];
  int  n = serialize_tx_data_request(content, sizeof(content), msg);
  record("TX_DATA_REQUEST", direction, msg.slot.sfn(), msg.slot.slot_index(), total_size, n, lat_ns, content);
}


inline void record_fapi(const char* direction, const fapi::slot_indication& msg, int total_size, uint64_t lat_ns = 0)
{
  char content[MAX_MESSAGE_CONTENT_LEN];
  int  n = serialize_slot_indication(content, sizeof(content), msg);
  record("SLOT_INDICATION", direction, msg.slot.sfn(), msg.slot.slot_index(), total_size, 1, lat_ns, content);
  (void)n;
}

inline void record_fapi(const char* direction, const fapi::rx_data_indication& msg, int total_size, uint64_t lat_ns = 0)
{
  char content[MAX_MESSAGE_CONTENT_LEN];
  int  n = serialize_rx_data_indication(content, sizeof(content), msg);
  record("RX_DATA_INDICATION", direction, msg.slot.sfn(), msg.slot.slot_index(), total_size, n, lat_ns, content);
}

inline void record_fapi(const char* direction, const fapi::crc_indication& msg, int total_size, uint64_t lat_ns = 0)
{
  char content[MAX_MESSAGE_CONTENT_LEN];
  int  n = serialize_crc_indication(content, sizeof(content), msg);
  record("CRC_INDICATION", direction, msg.slot.sfn(), msg.slot.slot_index(), total_size, n, lat_ns, content);
}

inline void record_fapi(const char* direction, const fapi::uci_indication& msg, int total_size, uint64_t lat_ns = 0)
{
  char content[MAX_MESSAGE_CONTENT_LEN];
  int  n = serialize_uci_indication(content, sizeof(content), msg);
  record("UCI_INDICATION", direction, msg.slot.sfn(), msg.slot.slot_index(), total_size, n, lat_ns, content);
}

inline void record_fapi(const char* direction, const fapi::srs_indication& msg, int total_size, uint64_t lat_ns = 0)
{
  char content[MAX_MESSAGE_CONTENT_LEN];
  int  n = serialize_srs_indication(content, sizeof(content), msg);
  record("SRS_INDICATION", direction, msg.slot.sfn(), msg.slot.slot_index(), total_size, n, lat_ns, content);
}

inline void record_fapi(const char* direction, const fapi::rach_indication& msg, int total_size, uint64_t lat_ns = 0)
{
  char content[MAX_MESSAGE_CONTENT_LEN];
  int  n = serialize_rach_indication(content, sizeof(content), msg);
  record("RACH_INDICATION", direction, msg.slot.sfn(), msg.slot.slot_index(), total_size, n, lat_ns, content);
}


inline void record_fapi_last_message(const char* direction, slot_point slot, int total_size, uint64_t lat_ns = 0)
{
  char content[MAX_MESSAGE_CONTENT_LEN];
  int  n = serialize_last_message(content, sizeof(content), slot);
  record("P7_LAST_MESSAGE", direction, slot.sfn(), slot.slot_index(), total_size, n, lat_ns, content);
}

} // namespace ocudu::fapi_stats
