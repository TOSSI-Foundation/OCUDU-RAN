#pragma once

#include "fapi_serializer_common.h"
#include "ocudu/fapi/cell_config.h"
#include "ocudu/fapi/p5/p5_messages.h"

namespace ocudu {
namespace fapi_serial {

void serialize(buffer_writer& w, const fapi::param_request& msg);
void deserialize(buffer_reader& r, fapi::param_request& msg);

void serialize(buffer_writer& w, const fapi::param_response& msg);
void deserialize(buffer_reader& r, fapi::param_response& msg);

void serialize(buffer_writer& w, const fapi::config_response& msg);
void deserialize(buffer_reader& r, fapi::config_response& msg);

void serialize(buffer_writer& w, const fapi::start_request& msg);
void deserialize(buffer_reader& r, fapi::start_request& msg);

void serialize(buffer_writer& w, const fapi::stop_request& msg);
void deserialize(buffer_reader& r, fapi::stop_request& msg);

void serialize(buffer_writer& w, const fapi::stop_indication& msg);
void deserialize(buffer_reader& r, fapi::stop_indication& msg);

void serialize(buffer_writer& w, const fapi::carrier_config& cfg);
void deserialize(buffer_reader& r, fapi::carrier_config& cfg);

void serialize(buffer_writer& w, const rach_config_common& cfg);
void deserialize(buffer_reader& r, rach_config_common& cfg);

void serialize(buffer_writer& w, const ssb_configuration& cfg);
void deserialize(buffer_reader& r, ssb_configuration& cfg);

void serialize(buffer_writer& w, const fapi::cell_configuration& cfg);
void deserialize(buffer_reader& r, fapi::cell_configuration& cfg);

void serialize(buffer_writer& w, const fapi::config_request& msg);
void deserialize(buffer_reader& r, fapi::config_request& msg);

} // namespace fapi_serial
} // namespace ocudu
