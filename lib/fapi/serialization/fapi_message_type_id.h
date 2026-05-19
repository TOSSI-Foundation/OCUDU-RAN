#pragma once

#include <cstdint>

namespace ocudu {
namespace fapi_serial {

/// FAPI message type identifiers.
/// Values match 3GPP 5G NR FAPI specification.
struct fapi_msg_type {
  static constexpr uint8_t PARAM_REQUEST    = 0x00;
  static constexpr uint8_t PARAM_RESPONSE   = 0x01;
  static constexpr uint8_t CONFIG_REQUEST   = 0x02;
  static constexpr uint8_t CONFIG_RESPONSE  = 0x03;
  static constexpr uint8_t START_REQUEST    = 0x04;
  static constexpr uint8_t STOP_REQUEST     = 0x05;
  static constexpr uint8_t STOP_INDICATION  = 0x06;
  static constexpr uint8_t ERROR_INDICATION = 0x07;

  static constexpr uint8_t DL_TTI_REQUEST     = 0x80;
  static constexpr uint8_t UL_TTI_REQUEST     = 0x81;
  static constexpr uint8_t SLOT_INDICATION    = 0x82;
  static constexpr uint8_t UL_DCI_REQUEST     = 0x83;
  static constexpr uint8_t TX_DATA_REQUEST    = 0x84;
  static constexpr uint8_t RX_DATA_INDICATION = 0x85;
  static constexpr uint8_t CRC_INDICATION     = 0x86;
  static constexpr uint8_t UCI_INDICATION     = 0x87;
  static constexpr uint8_t SRS_INDICATION     = 0x88;
  static constexpr uint8_t RACH_INDICATION    = 0x89;

  static constexpr uint8_t VENDOR_MSG_HEADER_IND = 0x1A;
};

/// Returns a human-readable name for a FAPI message type ID.
inline const char* fapi_msg_type_to_string(uint8_t msg_type)
{
  switch (msg_type) {
    case fapi_msg_type::PARAM_REQUEST:       return "PARAM.request";
    case fapi_msg_type::PARAM_RESPONSE:      return "PARAM.response";
    case fapi_msg_type::CONFIG_REQUEST:      return "CONFIG.request";
    case fapi_msg_type::CONFIG_RESPONSE:     return "CONFIG.response";
    case fapi_msg_type::START_REQUEST:       return "START.request";
    case fapi_msg_type::STOP_REQUEST:        return "STOP.request";
    case fapi_msg_type::STOP_INDICATION:     return "STOP.indication";
    case fapi_msg_type::ERROR_INDICATION:    return "ERROR.indication";
    case fapi_msg_type::DL_TTI_REQUEST:      return "DL_TTI.request";
    case fapi_msg_type::UL_TTI_REQUEST:      return "UL_TTI.request";
    case fapi_msg_type::SLOT_INDICATION:     return "SLOT.indication";
    case fapi_msg_type::UL_DCI_REQUEST:      return "UL_DCI.request";
    case fapi_msg_type::TX_DATA_REQUEST:     return "TX_DATA.request";
    case fapi_msg_type::RX_DATA_INDICATION:  return "RX_DATA.indication";
    case fapi_msg_type::CRC_INDICATION:      return "CRC.indication";
    case fapi_msg_type::UCI_INDICATION:      return "UCI.indication";
    case fapi_msg_type::SRS_INDICATION:      return "SRS.indication";
    case fapi_msg_type::RACH_INDICATION:     return "RACH.indication";
    case fapi_msg_type::VENDOR_MSG_HEADER_IND: return "VENDOR_MSG_HEADER";
    default:                                 return "UNKNOWN";
  }
}

} // namespace fapi_serial
} // namespace ocudu
