// SPDX-FileCopyrightText: Copyright (C) 2021-2026 Software Radio Systems Limited
// SPDX-License-Identifier: BSD-3-Clause-Open-MPI
// Portions of this file may implement 3GPP specifications, which may be subject to additional licensing requirements.

#include "f1ap_du_trp_information_exchange_procedure.h"
#include "../../asn1_helpers.h"
#include "ocudu/adt/format.h"
#include "ocudu/asn1/f1ap/common.h"
#include "ocudu/asn1/f1ap/f1ap_pdu_contents.h"
#include "ocudu/f1ap/du/f1ap_du.h"
#include "ocudu/f1ap/du/f1ap_du_positioning_handler.h"
#include "ocudu/f1ap/f1ap_message.h"
#include "ocudu/f1ap/f1ap_message_notifier.h"

using namespace ocudu;
using namespace odu;
using namespace asn1::f1ap;

f1ap_du_trp_information_exchange_procedure::f1ap_du_trp_information_exchange_procedure(
    const trp_info_request_s&    msg_,
    f1ap_du_positioning_handler& du_mng_,
    f1ap_du_time_provider&       time_provider_,
    f1ap_message_notifier&       cu_notifier_) :
  msg(msg_), du_mng(du_mng_), time_provider(time_provider_), cu_notifier(cu_notifier_), logger(ocudulog::fetch_basic_logger("DU-F1"))
{
}

void f1ap_du_trp_information_exchange_procedure::operator()(coro_context<async_task<void>>& ctx)
{
  CORO_BEGIN(ctx);

  if (not validate_request()) {
    send_failure();
    CORO_EARLY_RETURN();
  }

  // Request TRPs from DU.
  du_trp_info_response du_resp = du_mng.request_trp_info();

  if (du_resp.trps.empty()) {
    logger.debug("TRP information exchange procedure failed: no TRPs found");
    send_failure();
    CORO_EARLY_RETURN();
  }

  // SFN Initialisation Time (TS 38.473 9.3.1.183): the system time of the start of SFN 0, from the latest slot to
  // time mapping. It lets the location server turn the (SFN, slot) timestamps of measurements into absolute time,
  // which single-satellite NTN Multi-RTT needs to know where the satellite was (TS 38.305 8.10).
  if (auto m = time_provider.get_last_mapping(subcarrier_spacing::kHz15); m.has_value() and m->ref_slot.valid()) {
    using namespace std::chrono;
    const auto since_sfn0 = nanoseconds{(m->ref_slot.sfn() * 10 + m->ref_slot.subframe_index()) * 1'000'000LL +
                                        m->ref_slot.subframe_slot_index() * 1'000'000LL /
                                            m->ref_slot.nof_slots_per_subframe()};
    const auto     sfn0     = duration_cast<nanoseconds>((m->time_point - since_sfn0).time_since_epoch());
    constexpr auto from1900 = 2208988800ULL; // 1900-01-01 to 1970-01-01, s
    const uint64_t secs     = static_cast<uint64_t>(duration_cast<seconds>(sfn0).count()) + from1900;
    const uint64_t frac = (static_cast<uint64_t>((sfn0 - duration_cast<seconds>(sfn0)).count()) << 32) / 1'000'000'000ULL;
    for (auto& trp : du_resp.trps) {
      trp.sfn_init_time = (secs << 32) | frac;
    }
  }

  // Send response back to CU-CP.
  send_response(du_resp);

  CORO_RETURN();
}

bool f1ap_du_trp_information_exchange_procedure::validate_request() const
{
  if (msg->trp_list_present) {
    logger.error("TRP list in TRP information request not supported");
    return false;
  }

  return true;
}

void f1ap_du_trp_information_exchange_procedure::send_response(const du_trp_info_response& du_resp) const
{
  f1ap_message resp_msg;

  resp_msg.pdu.set_successful_outcome().load_info_obj(ASN1_F1AP_ID_TRP_INFO_EXCHANGE);
  auto& resp = resp_msg.pdu.successful_outcome().value.trp_info_resp();

  resp->transaction_id = msg->transaction_id;

  resp->trp_info_list_trp_resp.resize(du_resp.trps.size());
  for (unsigned i = 0, e = du_resp.trps.size(); i != e; ++i) {
    resp->trp_info_list_trp_resp[i].load_info_obj(ASN1_F1AP_ID_TRP_INFO_ITEM);
    trp_info_s& asn1_trp_info = resp->trp_info_list_trp_resp[i].value().trp_info_item().trp_info;
    asn1_trp_info             = trp_info_to_asn1(du_resp.trps[i]);
  }

  // Send response to CU-CP.
  cu_notifier.on_new_message(resp_msg);
}

void f1ap_du_trp_information_exchange_procedure::send_failure() const
{
  f1ap_message fail_msg;

  fail_msg.pdu.set_unsuccessful_outcome().load_info_obj(ASN1_F1AP_ID_TRP_INFO_EXCHANGE);
  auto& fail = fail_msg.pdu.unsuccessful_outcome().value.trp_info_fail();

  fail->transaction_id         = msg->transaction_id;
  fail->cause.set_misc().value = cause_misc_opts::unspecified;

  // Send response to CU-CP.
  cu_notifier.on_new_message(fail_msg);
}
