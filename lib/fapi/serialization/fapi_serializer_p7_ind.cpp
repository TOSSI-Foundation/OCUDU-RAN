#include "fapi_serializer_p7_ind.h"

using namespace ocudu;
using namespace ocudu::fapi_serial;


void fapi_serial::serialize(buffer_writer& w, const fapi::slot_indication& msg)
{
  serialize(w, msg.slot);
  serialize(w, msg.time_point);
}

void fapi_serial::deserialize(buffer_reader& r, fapi::slot_indication& msg)
{
  deserialize(r, msg.slot);
  deserialize(r, msg.time_point);
}


void fapi_serial::serialize(buffer_writer& w, const fapi::rx_data_indication_pdu& pdu)
{
  w.write_u32(pdu.handle);
  serialize(w, pdu.rnti);
  serialize(w, pdu.harq_id);
  serialize_span_u8(w, pdu.transport_block);
}

void fapi_serial::deserialize(buffer_reader& r, fapi::rx_data_indication_pdu& pdu, std::vector<uint8_t>& tb_storage)
{
  pdu.handle = r.read_u32();
  deserialize(r, pdu.rnti);
  deserialize(r, pdu.harq_id);
  deserialize_span_u8(r, tb_storage, pdu.transport_block);
}


void fapi_serial::serialize(buffer_writer& w, const fapi::rx_data_indication& msg)
{
  serialize(w, msg.slot);
  uint16_t count = static_cast<uint16_t>(msg.pdus.size());
  w.write_u16(count);
  for (uint16_t i = 0; i < count; ++i) {
    serialize(w, msg.pdus[i]);
  }
}

void fapi_serial::deserialize(buffer_reader& r, fapi::rx_data_indication& msg,
                              std::vector<std::vector<uint8_t>>& tb_storages)
{
  deserialize(r, msg.slot);
  uint16_t count = r.read_u16();
  msg.pdus.clear();
  tb_storages.resize(count);
  for (uint16_t i = 0; i < count; ++i) {
    fapi::rx_data_indication_pdu pdu{};
    deserialize(r, pdu, tb_storages[i]);
    msg.pdus.push_back(std::move(pdu));
  }
}


void fapi_serial::serialize(buffer_writer& w, const fapi::crc_ind_pdu& pdu)
{
  w.write_u32(pdu.handle);
  serialize(w, pdu.rnti);
  serialize(w, pdu.harq_id);
  w.write_bool(pdu.tb_crc_status_ok);
  w.write_i16(pdu.ul_sinr_metric);
  serialize_optional(w, pdu.timing_advance_offset);
  w.write_u16(pdu.rssi);
  w.write_u16(pdu.rsrp);
}

void fapi_serial::deserialize(buffer_reader& r, fapi::crc_ind_pdu& pdu)
{
  pdu.handle = r.read_u32();
  deserialize(r, pdu.rnti);
  deserialize(r, pdu.harq_id);
  pdu.tb_crc_status_ok = r.read_bool();
  pdu.ul_sinr_metric   = r.read_i16();
  deserialize_optional(r, pdu.timing_advance_offset);
  pdu.rssi = r.read_u16();
  pdu.rsrp = r.read_u16();
}


void fapi_serial::serialize(buffer_writer& w, const fapi::crc_indication& msg)
{
  serialize(w, msg.slot);
  uint16_t count = static_cast<uint16_t>(msg.pdus.size());
  w.write_u16(count);
  for (uint16_t i = 0; i < count; ++i) {
    serialize(w, msg.pdus[i]);
  }
}

void fapi_serial::deserialize(buffer_reader& r, fapi::crc_indication& msg)
{
  deserialize(r, msg.slot);
  uint16_t count = r.read_u16();
  msg.pdus.clear();
  for (uint16_t i = 0; i < count; ++i) {
    fapi::crc_ind_pdu pdu{};
    deserialize(r, pdu);
    msg.pdus.push_back(std::move(pdu));
  }
}


void fapi_serial::serialize(buffer_writer& w, const fapi::sr_pdu_format_0_1& sr)
{
  w.write_bool(sr.sr_detected);
}

void fapi_serial::deserialize(buffer_reader& r, fapi::sr_pdu_format_0_1& sr)
{
  sr.sr_detected = r.read_bool();
}

void fapi_serial::serialize(buffer_writer& w, const fapi::uci_harq_format_0_1& harq)
{
  uint16_t count = static_cast<uint16_t>(harq.harq_values.size());
  w.write_u16(count);
  for (uint16_t i = 0; i < count; ++i) {
    serialize_enum_u8(w, harq.harq_values[i]);
  }
}

void fapi_serial::deserialize(buffer_reader& r, fapi::uci_harq_format_0_1& harq)
{
  uint16_t count = r.read_u16();
  harq.harq_values.clear();
  for (uint16_t i = 0; i < count; ++i) {
    uci_pucch_f0_or_f1_harq_values val;
    deserialize_enum_u8(r, val);
    harq.harq_values.push_back(val);
  }
}

void fapi_serial::serialize(buffer_writer& w, const fapi::uci_harq_pdu& harq)
{
  serialize_enum_u8(w, harq.detection_status);
  serialize(w, harq.expected_bit_length);
  serialize(w, harq.payload);
}

void fapi_serial::deserialize(buffer_reader& r, fapi::uci_harq_pdu& harq)
{
  deserialize_enum_u8(r, harq.detection_status);
  deserialize(r, harq.expected_bit_length);
  deserialize(r, harq.payload);
}

void fapi_serial::serialize(buffer_writer& w, const fapi::uci_csi_part1& csi)
{
  serialize_enum_u8(w, csi.detection_status);
  serialize(w, csi.expected_bit_length);
  serialize(w, csi.payload);
}

void fapi_serial::deserialize(buffer_reader& r, fapi::uci_csi_part1& csi)
{
  deserialize_enum_u8(r, csi.detection_status);
  deserialize(r, csi.expected_bit_length);
  deserialize(r, csi.payload);
}

void fapi_serial::serialize(buffer_writer& w, const fapi::uci_csi_part2& csi)
{
  serialize_enum_u8(w, csi.detection_status);
  serialize(w, csi.expected_bit_length);
  serialize(w, csi.payload);
}

void fapi_serial::deserialize(buffer_reader& r, fapi::uci_csi_part2& csi)
{
  deserialize_enum_u8(r, csi.detection_status);
  deserialize(r, csi.expected_bit_length);
  deserialize(r, csi.payload);
}

void fapi_serial::serialize(buffer_writer& w, const fapi::sr_pdu_format_2_3_4& sr)
{
  serialize(w, sr.sr_payload);
}

void fapi_serial::deserialize(buffer_reader& r, fapi::sr_pdu_format_2_3_4& sr)
{
  deserialize(r, sr.sr_payload);
}


void fapi_serial::serialize(buffer_writer& w, const fapi::uci_pusch_pdu& pdu)
{
  w.write_u32(pdu.handle);
  serialize(w, pdu.rnti);
  w.write_i16(pdu.ul_sinr_metric);
  serialize_optional(w, pdu.timing_advance_offset);
  w.write_u16(pdu.rssi);
  w.write_u16(pdu.rsrp);
  serialize_optional(w, pdu.harq);
  serialize_optional(w, pdu.csi_part1);
  serialize_optional(w, pdu.csi_part2);
}

void fapi_serial::deserialize(buffer_reader& r, fapi::uci_pusch_pdu& pdu)
{
  pdu.handle = r.read_u32();
  deserialize(r, pdu.rnti);
  pdu.ul_sinr_metric = r.read_i16();
  deserialize_optional(r, pdu.timing_advance_offset);
  pdu.rssi = r.read_u16();
  pdu.rsrp = r.read_u16();
  deserialize_optional(r, pdu.harq);
  deserialize_optional(r, pdu.csi_part1);
  deserialize_optional(r, pdu.csi_part2);
}


void fapi_serial::serialize(buffer_writer& w, const fapi::uci_pucch_pdu_format_0_1& pdu)
{
  w.write_u32(pdu.handle);
  serialize(w, pdu.rnti);
  serialize_enum_u8(w, pdu.pucch_format);
  w.write_i16(pdu.ul_sinr_metric);
  serialize_optional(w, pdu.timing_advance_offset);
  w.write_u16(pdu.rssi);
  w.write_u16(pdu.rsrp);
  serialize_optional(w, pdu.sr);
  serialize_optional(w, pdu.harq);
}

void fapi_serial::deserialize(buffer_reader& r, fapi::uci_pucch_pdu_format_0_1& pdu)
{
  pdu.handle = r.read_u32();
  deserialize(r, pdu.rnti);
  deserialize_enum_u8(r, pdu.pucch_format);
  pdu.ul_sinr_metric = r.read_i16();
  deserialize_optional(r, pdu.timing_advance_offset);
  pdu.rssi = r.read_u16();
  pdu.rsrp = r.read_u16();
  deserialize_optional(r, pdu.sr);
  deserialize_optional(r, pdu.harq);
}


void fapi_serial::serialize(buffer_writer& w, const fapi::uci_pucch_pdu_format_2_3_4& pdu)
{
  w.write_u32(pdu.handle);
  serialize(w, pdu.rnti);
  serialize_enum_u8(w, pdu.pucch_format);
  w.write_i16(pdu.ul_sinr_metric);
  serialize_optional(w, pdu.timing_advance_offset);
  w.write_u16(pdu.rssi);
  w.write_u16(pdu.rsrp);
  serialize_optional(w, pdu.sr);
  serialize_optional(w, pdu.harq);
  serialize_optional(w, pdu.csi_part1);
  serialize_optional(w, pdu.csi_part2);
}

void fapi_serial::deserialize(buffer_reader& r, fapi::uci_pucch_pdu_format_2_3_4& pdu)
{
  pdu.handle = r.read_u32();
  deserialize(r, pdu.rnti);
  deserialize_enum_u8(r, pdu.pucch_format);
  pdu.ul_sinr_metric = r.read_i16();
  deserialize_optional(r, pdu.timing_advance_offset);
  pdu.rssi = r.read_u16();
  pdu.rsrp = r.read_u16();
  deserialize_optional(r, pdu.sr);
  deserialize_optional(r, pdu.harq);
  deserialize_optional(r, pdu.csi_part1);
  deserialize_optional(r, pdu.csi_part2);
}

void fapi_serial::serialize(buffer_writer& w, const fapi::uci_indication& msg)
{
  serialize(w, msg.slot);
  uint16_t count = static_cast<uint16_t>(msg.pdus.size());
  w.write_u16(count);
  for (uint16_t i = 0; i < count; ++i) {
    const auto& variant_pdu = msg.pdus[i];
    uint8_t variant_idx = static_cast<uint8_t>(variant_pdu.index());
    w.write_u8(variant_idx);
    std::visit([&w](const auto& pdu) { serialize(w, pdu); }, variant_pdu);
  }
}

void fapi_serial::deserialize(buffer_reader& r, fapi::uci_indication& msg)
{
  deserialize(r, msg.slot);
  uint16_t count = r.read_u16();
  msg.pdus.clear();
  for (uint16_t i = 0; i < count; ++i) {
    uint8_t variant_idx = r.read_u8();
    switch (variant_idx) {
      case 0: {
        fapi::uci_pusch_pdu pdu{};
        deserialize(r, pdu);
        msg.pdus.push_back(std::move(pdu));
        break;
      }
      case 1: {
        fapi::uci_pucch_pdu_format_0_1 pdu{};
        deserialize(r, pdu);
        msg.pdus.push_back(std::move(pdu));
        break;
      }
      case 2: {
        fapi::uci_pucch_pdu_format_2_3_4 pdu{};
        deserialize(r, pdu);
        msg.pdus.push_back(std::move(pdu));
        break;
      }
      default:
        throw std::runtime_error("Unknown uci_indication variant index: " + std::to_string(variant_idx));
    }
  }
}

void fapi_serial::serialize(buffer_writer& w, const srs_channel_matrix& matrix)
{
  uint16_t nof_rx = static_cast<uint16_t>(matrix.get_nof_rx_ports());
  uint16_t nof_tx = static_cast<uint16_t>(matrix.get_nof_tx_ports());
  w.write_u16(nof_rx);
  w.write_u16(nof_tx);
  for (unsigned rx = 0; rx < nof_rx; ++rx) {
    for (unsigned tx = 0; tx < nof_tx; ++tx) {
      cf_t coeff = matrix.get_coefficient(rx, tx);
      w.write_float(coeff.real());
      w.write_float(coeff.imag());
    }
  }
}

void fapi_serial::deserialize(buffer_reader& r, srs_channel_matrix& matrix)
{
  uint16_t nof_rx = r.read_u16();
  uint16_t nof_tx = r.read_u16();
  matrix = srs_channel_matrix(nof_rx, nof_tx);
  for (unsigned rx = 0; rx < nof_rx; ++rx) {
    for (unsigned tx = 0; tx < nof_tx; ++tx) {
      float real_part = r.read_float();
      float imag_part = r.read_float();
      matrix.set_coefficient(cf_t(real_part, imag_part), rx, tx);
    }
  }
}


void fapi_serial::serialize(buffer_writer& w, const fapi::srs_positioning_report& report)
{
  serialize_optional(w, report.ul_relative_toa);
  if (report.rsrp.has_value()) {
    w.write_u8(1);
    w.write_float(report.rsrp.value());
  } else {
    w.write_u8(0);
  }
}

void fapi_serial::deserialize(buffer_reader& r, fapi::srs_positioning_report& report)
{
  deserialize_optional(r, report.ul_relative_toa);
  uint8_t has_rsrp = r.read_u8();
  if (has_rsrp) {
    report.rsrp = r.read_float();
  } else {
    report.rsrp = std::nullopt;
  }
}


void fapi_serial::serialize(buffer_writer& w, const fapi::srs_indication_pdu& pdu)
{
  w.write_u32(pdu.handle);
  serialize(w, pdu.rnti);
  serialize_optional(w, pdu.timing_advance_offset);
  if (pdu.matrix.has_value()) {
    w.write_u8(1);
    serialize(w, pdu.matrix.value());
  } else {
    w.write_u8(0);
  }
  if (pdu.positioning.has_value()) {
    w.write_u8(1);
    serialize(w, pdu.positioning.value());
  } else {
    w.write_u8(0);
  }
}

void fapi_serial::deserialize(buffer_reader& r, fapi::srs_indication_pdu& pdu)
{
  pdu.handle = r.read_u32();
  deserialize(r, pdu.rnti);
  deserialize_optional(r, pdu.timing_advance_offset);
  uint8_t has_matrix = r.read_u8();
  if (has_matrix) {
    srs_channel_matrix mat;
    deserialize(r, mat);
    pdu.matrix = std::move(mat);
  } else {
    pdu.matrix = std::nullopt;
  }
  uint8_t has_positioning = r.read_u8();
  if (has_positioning) {
    fapi::srs_positioning_report report;
    deserialize(r, report);
    pdu.positioning = std::move(report);
  } else {
    pdu.positioning = std::nullopt;
  }
}


void fapi_serial::serialize(buffer_writer& w, const fapi::srs_indication& msg)
{
  serialize(w, msg.slot);
  uint16_t count = static_cast<uint16_t>(msg.pdus.size());
  w.write_u16(count);
  for (uint16_t i = 0; i < count; ++i) {
    serialize(w, msg.pdus[i]);
  }
}

void fapi_serial::deserialize(buffer_reader& r, fapi::srs_indication& msg)
{
  deserialize(r, msg.slot);
  uint16_t count = r.read_u16();
  msg.pdus.clear();
  for (uint16_t i = 0; i < count; ++i) {
    fapi::srs_indication_pdu pdu{};
    deserialize(r, pdu);
    msg.pdus.push_back(std::move(pdu));
  }
}


void fapi_serial::serialize(buffer_writer& w, const fapi::rach_indication_pdu_preamble& preamble)
{
  w.write_u8(preamble.preamble_index);
  serialize_optional(w, preamble.timing_advance_offset);
  w.write_u32(preamble.preamble_pwr);
  w.write_u8(preamble.preamble_snr);
}

void fapi_serial::deserialize(buffer_reader& r, fapi::rach_indication_pdu_preamble& preamble)
{
  preamble.preamble_index = r.read_u8();
  deserialize_optional(r, preamble.timing_advance_offset);
  preamble.preamble_pwr = r.read_u32();
  preamble.preamble_snr = r.read_u8();
}


void fapi_serial::serialize(buffer_writer& w, const fapi::rach_indication_pdu& pdu)
{
  w.write_u32(pdu.handle);
  w.write_u8(pdu.symbol_index);
  w.write_u8(pdu.slot_index);
  w.write_u8(pdu.ra_index);
  w.write_u32(pdu.avg_rssi);
  w.write_u8(pdu.avg_snr);
  uint16_t count = static_cast<uint16_t>(pdu.preambles.size());
  w.write_u16(count);
  for (uint16_t i = 0; i < count; ++i) {
    serialize(w, pdu.preambles[i]);
  }
}

void fapi_serial::deserialize(buffer_reader& r, fapi::rach_indication_pdu& pdu)
{
  pdu.handle       = r.read_u32();
  pdu.symbol_index = r.read_u8();
  pdu.slot_index   = r.read_u8();
  pdu.ra_index     = r.read_u8();
  pdu.avg_rssi     = r.read_u32();
  pdu.avg_snr      = r.read_u8();
  uint16_t count   = r.read_u16();
  pdu.preambles.clear();
  for (uint16_t i = 0; i < count; ++i) {
    fapi::rach_indication_pdu_preamble preamble{};
    deserialize(r, preamble);
    pdu.preambles.push_back(std::move(preamble));
  }
}


void fapi_serial::serialize(buffer_writer& w, const fapi::rach_indication& msg)
{
  serialize(w, msg.slot);
  uint16_t count = static_cast<uint16_t>(msg.pdus.size());
  w.write_u16(count);
  for (uint16_t i = 0; i < count; ++i) {
    serialize(w, msg.pdus[i]);
  }
}

void fapi_serial::deserialize(buffer_reader& r, fapi::rach_indication& msg)
{
  deserialize(r, msg.slot);
  uint16_t count = r.read_u16();
  msg.pdus.clear();
  for (uint16_t i = 0; i < count; ++i) {
    fapi::rach_indication_pdu pdu{};
    deserialize(r, pdu);
    msg.pdus.push_back(std::move(pdu));
  }
}


void fapi_serial::serialize(buffer_writer& w, const fapi::error_indication& msg)
{
  serialize_optional(w, msg.slot);
  serialize_enum_u16(w, msg.message_id);
  serialize_enum_u8(w, msg.error_code);
  serialize_optional(w, msg.expected_slot);
}

void fapi_serial::deserialize(buffer_reader& r, fapi::error_indication& msg)
{
  deserialize_optional(r, msg.slot);
  deserialize_enum_u16(r, msg.message_id);
  deserialize_enum_u8(r, msg.error_code);
  deserialize_optional(r, msg.expected_slot);
}
