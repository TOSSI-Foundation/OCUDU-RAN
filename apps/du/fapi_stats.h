#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <string>

namespace ocudu::fapi_stats {

constexpr size_t MAX_MESSAGE_STATS       = 100000;
constexpr size_t MAX_MESSAGE_TYPE_LEN    = 64;
constexpr size_t MAX_DIRECTION_LEN       = 16;
constexpr size_t MAX_MESSAGE_CONTENT_LEN = 8192;


struct message_stat {
  uint64_t timestamp_ns                             = 0;
  uint64_t ipc_latency_ns                           = 0;
  char     message_type[MAX_MESSAGE_TYPE_LEN]       = {};
  char     direction[MAX_DIRECTION_LEN]             = {};
  int32_t  sfn                                      = 0;
  int32_t  slot                                     = 0;
  int32_t  pdu_size                                 = 0;
  int32_t  num_pdus                                 = 0;
  char     message_content[MAX_MESSAGE_CONTENT_LEN] = {};
};


void initialize(bool enabled, const std::string& output_path, bool add_timestamp = false);

bool is_enabled();


void record(const char* msg_type,
            const char* direction,
            int         sfn,
            int         slot,
            int         pdu_size,
            int         num_pdus,
            uint64_t    ipc_latency_ns,
            const char* content);

void dump_to_json();

void shutdown();

uint64_t timestamp_ns();

} // namespace ocudu::fapi_stats
