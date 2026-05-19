#pragma once

#include "fapi_serializer_common.h"
#include "ocudu/fapi/common/error_indication.h"
#include "ocudu/fapi/p7/messages/crc_indication.h"
#include "ocudu/fapi/p7/messages/rach_indication.h"
#include "ocudu/fapi/p7/messages/rx_data_indication.h"
#include "ocudu/fapi/p7/messages/slot_indication.h"
#include "ocudu/fapi/p7/messages/srs_indication.h"
#include "ocudu/fapi/p7/messages/uci_indication.h"

namespace ocudu {
namespace fapi_serial {


void serialize(buffer_writer& w, const fapi::slot_indication& msg);
void deserialize(buffer_reader& r, fapi::slot_indication& msg);


void serialize(buffer_writer& w, const fapi::rx_data_indication_pdu& pdu);
void deserialize(buffer_reader& r, fapi::rx_data_indication_pdu& pdu, std::vector<uint8_t>& tb_storage);

void serialize(buffer_writer& w, const fapi::rx_data_indication& msg);

void deserialize(buffer_reader& r, fapi::rx_data_indication& msg, std::vector<std::vector<uint8_t>>& tb_storages);


void serialize(buffer_writer& w, const fapi::crc_ind_pdu& pdu);
void deserialize(buffer_reader& r, fapi::crc_ind_pdu& pdu);

void serialize(buffer_writer& w, const fapi::crc_indication& msg);
void deserialize(buffer_reader& r, fapi::crc_indication& msg);

void serialize(buffer_writer& w, const fapi::sr_pdu_format_0_1& sr);
void deserialize(buffer_reader& r, fapi::sr_pdu_format_0_1& sr);

void serialize(buffer_writer& w, const fapi::uci_harq_format_0_1& harq);
void deserialize(buffer_reader& r, fapi::uci_harq_format_0_1& harq);

void serialize(buffer_writer& w, const fapi::uci_harq_pdu& harq);
void deserialize(buffer_reader& r, fapi::uci_harq_pdu& harq);

void serialize(buffer_writer& w, const fapi::uci_csi_part1& csi);
void deserialize(buffer_reader& r, fapi::uci_csi_part1& csi);

void serialize(buffer_writer& w, const fapi::uci_csi_part2& csi);
void deserialize(buffer_reader& r, fapi::uci_csi_part2& csi);

void serialize(buffer_writer& w, const fapi::sr_pdu_format_2_3_4& sr);
void deserialize(buffer_reader& r, fapi::sr_pdu_format_2_3_4& sr);

void serialize(buffer_writer& w, const fapi::uci_pusch_pdu& pdu);
void deserialize(buffer_reader& r, fapi::uci_pusch_pdu& pdu);

void serialize(buffer_writer& w, const fapi::uci_pucch_pdu_format_0_1& pdu);
void deserialize(buffer_reader& r, fapi::uci_pucch_pdu_format_0_1& pdu);

void serialize(buffer_writer& w, const fapi::uci_pucch_pdu_format_2_3_4& pdu);
void deserialize(buffer_reader& r, fapi::uci_pucch_pdu_format_2_3_4& pdu);

void serialize(buffer_writer& w, const fapi::uci_indication& msg);
void deserialize(buffer_reader& r, fapi::uci_indication& msg);

void serialize(buffer_writer& w, const srs_channel_matrix& matrix);
void deserialize(buffer_reader& r, srs_channel_matrix& matrix);

void serialize(buffer_writer& w, const fapi::srs_positioning_report& report);
void deserialize(buffer_reader& r, fapi::srs_positioning_report& report);

void serialize(buffer_writer& w, const fapi::srs_indication_pdu& pdu);
void deserialize(buffer_reader& r, fapi::srs_indication_pdu& pdu);

void serialize(buffer_writer& w, const fapi::srs_indication& msg);
void deserialize(buffer_reader& r, fapi::srs_indication& msg);

void serialize(buffer_writer& w, const fapi::rach_indication_pdu_preamble& preamble);
void deserialize(buffer_reader& r, fapi::rach_indication_pdu_preamble& preamble);

void serialize(buffer_writer& w, const fapi::rach_indication_pdu& pdu);
void deserialize(buffer_reader& r, fapi::rach_indication_pdu& pdu);

void serialize(buffer_writer& w, const fapi::rach_indication& msg);
void deserialize(buffer_reader& r, fapi::rach_indication& msg);


void serialize(buffer_writer& w, const fapi::error_indication& msg);
void deserialize(buffer_reader& r, fapi::error_indication& msg);

} // namespace fapi_serial
} // namespace ocudu
