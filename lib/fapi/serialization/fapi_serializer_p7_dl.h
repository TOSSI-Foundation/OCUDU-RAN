#pragma once

#include "fapi_serializer_common.h"
#include "ocudu/fapi/p7/messages/dl_tti_request.h"
#include "ocudu/fapi/p7/messages/tx_data_request.h"
#include "ocudu/fapi/p7/messages/ul_dci_request.h"

namespace ocudu {
namespace fapi_serial {

void serialize(buffer_writer& w, const fapi::tx_precoding_and_beamforming_pdu& pdu);
void deserialize(buffer_reader& r, fapi::tx_precoding_and_beamforming_pdu& pdu);

void serialize(buffer_writer& w, const fapi::dl_dci_pdu& pdu);
void deserialize(buffer_reader& r, fapi::dl_dci_pdu& pdu);

void serialize(buffer_writer& w, const fapi::dl_pdcch_pdu& pdu);
void deserialize(buffer_reader& r, fapi::dl_pdcch_pdu& pdu);

void serialize(buffer_writer& w, const fapi::dl_pdsch_codeword& cw);
void deserialize(buffer_reader& r, fapi::dl_pdsch_codeword& cw);

void serialize(buffer_writer& w, const fapi::dl_pdsch_maintenance_parameters_v3& params);
void deserialize(buffer_reader& r, fapi::dl_pdsch_maintenance_parameters_v3& params);

void serialize(buffer_writer& w, const fapi::dl_pdsch_parameters_v4& params);
void deserialize(buffer_reader& r, fapi::dl_pdsch_parameters_v4& params);

void serialize(buffer_writer& w, const fapi::dl_pdsch_pdu& pdu);
void deserialize(buffer_reader& r, fapi::dl_pdsch_pdu& pdu);

void serialize(buffer_writer& w, const fapi::dl_csi_rs_pdu& pdu);
void deserialize(buffer_reader& r, fapi::dl_csi_rs_pdu& pdu);

void serialize(buffer_writer& w, const fapi::dl_ssb_pdu& pdu);
void deserialize(buffer_reader& r, fapi::dl_ssb_pdu& pdu);

void serialize(buffer_writer& w, const fapi::dl_prs_pdu& pdu);
void deserialize(buffer_reader& r, fapi::dl_prs_pdu& pdu);

void serialize(buffer_writer& w, const fapi::dl_tti_request_pdu& pdu);
void deserialize(buffer_reader& r, fapi::dl_tti_request_pdu& pdu);

void serialize(buffer_writer& w, const fapi::dl_tti_request& msg);
void deserialize(buffer_reader& r, fapi::dl_tti_request& msg);

void serialize(buffer_writer& w, const fapi::tx_data_req_pdu& pdu);
void deserialize(buffer_reader& r, fapi::tx_data_req_pdu& pdu, std::vector<uint8_t>& tb_storage);

void serialize(buffer_writer& w, const fapi::tx_data_request& msg);
void deserialize(buffer_reader& r, fapi::tx_data_request& msg, std::vector<std::vector<uint8_t>>& tb_storages);

void serialize(buffer_writer& w, const fapi::ul_dci_pdu& pdu);
void deserialize(buffer_reader& r, fapi::ul_dci_pdu& pdu);

void serialize(buffer_writer& w, const fapi::ul_dci_request& msg);
void deserialize(buffer_reader& r, fapi::ul_dci_request& msg);

void serialize(buffer_writer& w, const pdcch_context& ctx);
void deserialize(buffer_reader& r, pdcch_context& ctx);

void serialize(buffer_writer& w, const pdsch_context& ctx);
void deserialize(buffer_reader& r, pdsch_context& ctx);

} // namespace fapi_serial
} // namespace ocudu
