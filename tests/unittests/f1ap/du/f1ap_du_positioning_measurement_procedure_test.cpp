// SPDX-FileCopyrightText: Copyright (C) 2021-2026 Software Radio Systems Limited
// SPDX-License-Identifier: BSD-3-Clause-Open-MPI
// Portions of this file may implement 3GPP specifications, which may be subject to additional licensing requirements.

#include "f1ap_du_test_helpers.h"
#include "tests/test_doubles/f1ap/f1ap_test_message_validators.h"
#include "tests/test_doubles/f1ap/f1ap_test_messages.h"
#include "ocudu/adt/format.h"
#include "ocudu/asn1/f1ap/common.h"
#include "ocudu/asn1/f1ap/f1ap_pdu_contents.h"
#include "ocudu/du/du_cell_config_helpers.h"
#include <gtest/gtest.h>

using namespace ocudu;
using namespace odu;

class f1ap_du_positioning_measurement_procedure_test : public f1ap_du_test
{
protected:
  f1ap_du_positioning_measurement_procedure_test()
  {
    // Test Preamble.
    run_f1_setup_procedure();
    run_f1ap_ue_create(test_ue_index);
    f1ap_message msg = test_helpers::generate_ue_context_setup_request(
        gnb_cu_ue_f1ap_id_t{0}, gnb_du_ue_f1ap_id_t{0}, 1, {}, config_helpers::make_default_du_cell_config().nr_cgi);
    run_ue_context_setup_procedure(test_ue_index, msg);

    this->f1c_gw.clear_tx_pdus();
  }

  du_ue_index_t test_ue_index = to_du_ue_index(0);
};

TEST_F(f1ap_du_positioning_measurement_procedure_test, when_valid_request_is_received_then_du_is_notified)
{
  f1ap_message req =
      test_helpers::generate_positioning_measurement_request({trp_id_t::min}, lmf_meas_id_t::min, ran_meas_id_t::min);

  this->f1ap->handle_message(req);

  ASSERT_TRUE(this->f1ap_du_cfg_handler.last_positioning_meas_request.has_value());
}

TEST_F(f1ap_du_positioning_measurement_procedure_test, when_invalid_request_is_received_then_failure_is_sent_to_cu)
{
  f1ap_message req =
      test_helpers::generate_positioning_measurement_request({trp_id_t::min},
                                                             lmf_meas_id_t::min,
                                                             ran_meas_id_t::min,
                                                             {asn1::f1ap::pos_meas_type_opts::options::ul_rtoa,
                                                              asn1::f1ap::pos_meas_type_opts::options::ul_srs_rsrp,
                                                              asn1::f1ap::pos_meas_type_opts::options::ul_aoa});

  // Remove SRS Configuration from the request to simulate an invalid request.
  req.pdu.init_msg().value.positioning_meas_request()->srs_configuration_present = false;

  this->f1ap->handle_message(req);

  // DU is not notified about the invalid request.
  ASSERT_FALSE(this->f1ap_du_cfg_handler.last_positioning_meas_request.has_value());

  auto tx_msg = this->f1c_gw.pop_tx_pdu();
  ASSERT_TRUE(test_helpers::is_valid_f1ap_positioning_measurement_failure(tx_msg.value()));
}

TEST_F(f1ap_du_positioning_measurement_procedure_test,
       when_positioning_measurement_succeeds_then_response_is_sent_to_cu)
{
  f1ap_message req =
      test_helpers::generate_positioning_measurement_request({trp_id_t::min}, lmf_meas_id_t::min, ran_meas_id_t::min);

  this->f1ap->handle_message(req);

  auto tx_msg = this->f1c_gw.pop_tx_pdu();
  ASSERT_TRUE(test_helpers::is_valid_f1ap_positioning_measurement_response(tx_msg.value()));
}

TEST_F(f1ap_du_positioning_measurement_procedure_test,
       when_gnb_rx_tx_is_requested_alone_then_request_is_accepted_and_result_is_packed)
{
  // gNB Rx-Tx on its own is a valid request: it is what Multi-RTT, and NTN network verification of UE location
  // (TS 38.305 Section 8.10.1), ask each gNB for.
  f1ap_message req = test_helpers::generate_positioning_measurement_request(
      {trp_id_t::min}, lmf_meas_id_t::min, ran_meas_id_t::min, {asn1::f1ap::pos_meas_type_opts::options::gnb_rx_tx});

  // A timing-aligned UE in an FR1 FDD cell, N_TA,offset = 25600 Tc: the DU reports -25600 Tc at k1,
  // i.e. ((-25600 + 985024) >> 1) + 1.
  static constexpr uint32_t reported_k1 = 479713;
  pos_meas_result           result{.trp_id = trp_id_t::min, .sl_rx = slot_point{subcarrier_spacing::kHz15, 0}};
  result.results.emplace_back(pos_meas_result_gnb_rx_tx{.granularity = 1, .gnb_rx_tx = reported_k1});
  this->f1ap_du_cfg_handler.next_positioning_meas_response.pos_meas_list.push_back(result);

  this->f1ap->handle_message(req);

  ASSERT_TRUE(this->f1ap_du_cfg_handler.last_positioning_meas_request.has_value());
  auto tx_msg = this->f1c_gw.pop_tx_pdu();
  ASSERT_TRUE(test_helpers::is_valid_f1ap_positioning_measurement_response(tx_msg.value()));

  const auto& resp = tx_msg.value().pdu.successful_outcome().value.positioning_meas_resp();
  ASSERT_EQ(resp->pos_meas_result_list.size(), 1U);
  ASSERT_EQ(resp->pos_meas_result_list[0].pos_meas_result.size(), 1U);
  const auto& value = resp->pos_meas_result_list[0].pos_meas_result[0].measured_results_value;
  ASSERT_EQ(value.type().value, asn1::f1ap::measured_results_value_c::types_opts::gnb_rx_tx_time_diff);
  ASSERT_EQ(value.gnb_rx_tx_time_diff().rx_tx_time_diff.type().value,
            asn1::f1ap::gnb_rx_tx_time_diff_meas_c::types_opts::k1);
  ASSERT_EQ(value.gnb_rx_tx_time_diff().rx_tx_time_diff.k1(), reported_k1);
}
