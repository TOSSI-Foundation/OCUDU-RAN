#pragma once

#include <cstdint>

namespace ocudu {
namespace fapi_serial {


struct fapi_xsm_msg_header {
  fapi_xsm_msg_header* p_next;              ///< 8 bytes: next in linked list (scatter-gather chain)
  fapi_xsm_msg_header* p_tx_data_elm_list;  ///< 8 bytes: TX data element sub-list
  uint8_t              msg_type;            ///< 1 byte:  FAPI message type ID
  uint8_t              num_messages_in_block; ///< 1 byte: number of messages in this block
  uint32_t             msg_len;             ///< 4 bytes: payload length after this header
  uint32_t             align_offset;        ///< 4 bytes: alignment offset
  uint64_t             time_stamp;          ///< 8 bytes: TX/RX timestamp in nanoseconds

  void* payload() { return reinterpret_cast<uint8_t*>(this) + sizeof(*this); }
  const void* payload() const { return reinterpret_cast<const uint8_t*>(this) + sizeof(*this); }
};

static_assert(sizeof(fapi_xsm_msg_header) <= 48, "fapi_xsm_msg_header must fit in expected size");

} // namespace fapi_serial
} // namespace ocudu
