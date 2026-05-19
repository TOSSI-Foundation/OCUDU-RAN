#pragma once

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
#include "ocudu/ran/slot_point.h"

namespace ocudu::fapi_stats {

int serialize_param_request(char* out, int max_len, const fapi::param_request& msg);
int serialize_param_response(char* out, int max_len, const fapi::param_response& msg);
int serialize_config_request(char* out, int max_len, const fapi::config_request& msg);
int serialize_config_response(char* out, int max_len, const fapi::config_response& msg);
int serialize_start_request(char* out, int max_len, const fapi::start_request& msg);
int serialize_stop_request(char* out, int max_len, const fapi::stop_request& msg);
int serialize_stop_indication(char* out, int max_len, const fapi::stop_indication& msg);
int serialize_error_indication(char* out, int max_len, const fapi::error_indication& msg);

int serialize_dl_tti_request(char* out, int max_len, const fapi::dl_tti_request& msg);
int serialize_ul_tti_request(char* out, int max_len, const fapi::ul_tti_request& msg);
int serialize_ul_dci_request(char* out, int max_len, const fapi::ul_dci_request& msg);
int serialize_tx_data_request(char* out, int max_len, const fapi::tx_data_request& msg);

int serialize_slot_indication(char* out, int max_len, const fapi::slot_indication& msg);
int serialize_rx_data_indication(char* out, int max_len, const fapi::rx_data_indication& msg);
int serialize_crc_indication(char* out, int max_len, const fapi::crc_indication& msg);
int serialize_uci_indication(char* out, int max_len, const fapi::uci_indication& msg);
int serialize_srs_indication(char* out, int max_len, const fapi::srs_indication& msg);
int serialize_rach_indication(char* out, int max_len, const fapi::rach_indication& msg);

int serialize_last_message(char* out, int max_len, slot_point slot);

} // namespace ocudu::fapi_stats
