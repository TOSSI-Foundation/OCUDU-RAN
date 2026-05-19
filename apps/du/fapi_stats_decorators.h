#pragma once

#include "fapi_stats.h"
#include "fapi_stats_record.h"
#include "ocudu/fapi/common/error_indication_notifier.h"
#include <cstdio>
#include "ocudu/fapi/p5/p5_requests_gateway.h"
#include "ocudu/fapi/p5/p5_responses_notifier.h"
#include "ocudu/fapi/p7/p7_indications_notifier.h"
#include "ocudu/fapi/p7/p7_last_request_notifier.h"
#include "ocudu/fapi/p7/p7_requests_gateway.h"
#include "ocudu/fapi/p7/p7_slot_indication_notifier.h"

namespace ocudu::fapi_stats {


class p5_requests_gateway_recorder final : public fapi::p5_requests_gateway
{
public:
  p5_requests_gateway_recorder(fapi::p5_requests_gateway& wrapped, const char* direction)
    : wrapped_(wrapped), direction_(direction) {}

  void send_param_request(const fapi::param_request& msg) override
  {
    if (is_enabled()) { record_fapi(direction_, msg, 0, 0); }
    wrapped_.send_param_request(msg);
  }
  void send_config_request(const fapi::config_request& msg) override
  {
    if (is_enabled()) { record_fapi(direction_, msg, 0, 0); }
    wrapped_.send_config_request(msg);
  }
  void send_start_request(const fapi::start_request& msg) override
  {
    if (is_enabled()) { record_fapi(direction_, msg, 0, 0); }
    wrapped_.send_start_request(msg);
  }
  void send_stop_request(const fapi::stop_request& msg) override
  {
    if (is_enabled()) { record_fapi(direction_, msg, 0, 0); }
    wrapped_.send_stop_request(msg);
  }

private:
  fapi::p5_requests_gateway& wrapped_;
  const char*                direction_;
};

class p7_requests_gateway_recorder final : public fapi::p7_requests_gateway
{
public:
  p7_requests_gateway_recorder(fapi::p7_requests_gateway& wrapped, const char* direction)
    : wrapped_(wrapped), direction_(direction) {}

  void send_dl_tti_request(const fapi::dl_tti_request& msg) override
  {
    if (is_enabled()) { record_fapi(direction_, msg, 0, 0); }
    wrapped_.send_dl_tti_request(msg);
  }
  void send_ul_tti_request(const fapi::ul_tti_request& msg) override
  {
    if (is_enabled()) { record_fapi(direction_, msg, 0, 0); }
    wrapped_.send_ul_tti_request(msg);
  }
  void send_ul_dci_request(const fapi::ul_dci_request& msg) override
  {
    if (is_enabled()) { record_fapi(direction_, msg, 0, 0); }
    wrapped_.send_ul_dci_request(msg);
  }
  void send_tx_data_request(const fapi::tx_data_request& msg) override
  {
    if (is_enabled()) { record_fapi(direction_, msg, 0, 0); }
    wrapped_.send_tx_data_request(msg);
  }

private:
  fapi::p7_requests_gateway& wrapped_;
  const char*                direction_;
};

class p7_last_request_notifier_recorder final : public fapi::p7_last_request_notifier
{
public:
  p7_last_request_notifier_recorder(fapi::p7_last_request_notifier& wrapped, const char* direction)
    : wrapped_(wrapped), direction_(direction) {}

  void on_last_message(slot_point slot) override
  {
    if (is_enabled()) { record_fapi_last_message(direction_, slot, 0, 0); }
    wrapped_.on_last_message(slot);
  }

private:
  fapi::p7_last_request_notifier& wrapped_;
  const char*                     direction_;
};



class p5_responses_notifier_recorder final : public fapi::p5_responses_notifier
{
public:
  p5_responses_notifier_recorder(fapi::p5_responses_notifier& wrapped, const char* direction)
    : wrapped_(wrapped), direction_(direction) {}

  void on_param_response(const fapi::param_response& msg) override
  {
    if (is_enabled()) { record_fapi(direction_, msg, 0, 0); }
    wrapped_.on_param_response(msg);
  }
  void on_config_response(const fapi::config_response& msg) override
  {
    if (is_enabled()) { record_fapi(direction_, msg, 0, 0); }
    wrapped_.on_config_response(msg);
  }
  void on_stop_indication(const fapi::stop_indication& msg) override
  {
    if (is_enabled()) { record_fapi(direction_, msg, 0, 0); }
    wrapped_.on_stop_indication(msg);
  }

private:
  fapi::p5_responses_notifier& wrapped_;
  const char*                  direction_;
};

class p7_indications_notifier_recorder final : public fapi::p7_indications_notifier
{
public:
  p7_indications_notifier_recorder(fapi::p7_indications_notifier& wrapped, const char* direction)
    : wrapped_(wrapped), direction_(direction) {}

  void on_rx_data_indication(const fapi::rx_data_indication& msg) override
  {
    if (is_enabled()) { record_fapi(direction_, msg, 0, 0); }
    wrapped_.on_rx_data_indication(msg);
  }
  void on_crc_indication(const fapi::crc_indication& msg) override
  {
    if (is_enabled()) { record_fapi(direction_, msg, 0, 0); }
    wrapped_.on_crc_indication(msg);
  }
  void on_uci_indication(const fapi::uci_indication& msg) override
  {
    if (is_enabled()) { record_fapi(direction_, msg, 0, 0); }
    wrapped_.on_uci_indication(msg);
  }
  void on_srs_indication(const fapi::srs_indication& msg) override
  {
    if (is_enabled()) { record_fapi(direction_, msg, 0, 0); }
    wrapped_.on_srs_indication(msg);
  }
  void on_rach_indication(const fapi::rach_indication& msg) override
  {
    if (is_enabled()) { record_fapi(direction_, msg, 0, 0); }
    wrapped_.on_rach_indication(msg);
  }

private:
  fapi::p7_indications_notifier& wrapped_;
  const char*                    direction_;
};

class p7_slot_indication_notifier_recorder final : public fapi::p7_slot_indication_notifier
{
public:
  p7_slot_indication_notifier_recorder(fapi::p7_slot_indication_notifier& wrapped, const char* direction)
    : wrapped_(wrapped), direction_(direction) {}

  void on_slot_indication(const fapi::slot_indication& msg) override
  {
    if (is_enabled()) { record_fapi(direction_, msg, 0, 0); }
    wrapped_.on_slot_indication(msg);
  }

private:
  fapi::p7_slot_indication_notifier& wrapped_;
  const char*                        direction_;
};

class error_indication_notifier_recorder final : public fapi::error_indication_notifier
{
public:
  error_indication_notifier_recorder(fapi::error_indication_notifier& wrapped, const char* direction)
    : wrapped_(wrapped), direction_(direction) {}

  void on_error_indication(const fapi::error_indication& msg) override
  {
    if (is_enabled()) { record_fapi(direction_, msg, 0, 0); }
    wrapped_.on_error_indication(msg);
  }

private:
  fapi::error_indication_notifier& wrapped_;
  const char*                      direction_;
};



struct recorders_storage {
  std::vector<std::unique_ptr<p5_requests_gateway_recorder>>          p5_gw;
  std::vector<std::unique_ptr<p7_requests_gateway_recorder>>          p7_gw;
  std::vector<std::unique_ptr<p7_last_request_notifier_recorder>>     p7_last_req;
  std::vector<std::unique_ptr<p5_responses_notifier_recorder>>        p5_resp;
  std::vector<std::unique_ptr<p7_indications_notifier_recorder>>      p7_ind;
  std::vector<std::unique_ptr<p7_slot_indication_notifier_recorder>>  p7_slot;
  std::vector<std::unique_ptr<error_indication_notifier_recorder>>    err_ind;
};

} // namespace ocudu::fapi_stats
