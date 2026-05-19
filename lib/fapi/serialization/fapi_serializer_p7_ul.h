#pragma once

#include "fapi_serializer_common.h"
#include "ocudu/fapi/p7/messages/ul_tti_request.h"

namespace ocudu {
namespace fapi_serial {

void serialize(buffer_writer& w, const fapi::ul_prach_pdu& pdu);
void deserialize(buffer_reader& r, fapi::ul_prach_pdu& pdu);

void serialize(buffer_writer& w, const fapi::ul_pusch_pdu& pdu);
void deserialize(buffer_reader& r, fapi::ul_pusch_pdu& pdu);

void serialize(buffer_writer& w, const fapi::ul_pucch_pdu& pdu);
void deserialize(buffer_reader& r, fapi::ul_pucch_pdu& pdu);

void serialize(buffer_writer& w, const fapi::ul_srs_pdu& pdu);
void deserialize(buffer_reader& r, fapi::ul_srs_pdu& pdu);

void serialize(buffer_writer& w, const fapi::ul_tti_request_pdu& pdu);
void deserialize(buffer_reader& r, fapi::ul_tti_request_pdu& pdu);

void serialize(buffer_writer& w, const fapi::ul_tti_request& msg);
void deserialize(buffer_reader& r, fapi::ul_tti_request& msg);

} // namespace fapi_serial
} // namespace ocudu
