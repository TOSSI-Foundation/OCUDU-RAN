#include "split6_o_du_low_plugin_xsm.h"

#include "apps/du/fapi_xsm_proxy.h"
#include "apps/du/fapi_xsm_transport.h"
#include "ocudu/fapi/common/error_indication_notifier.h"
#include "ocudu/fapi/p5/p5_responses_notifier.h"
#include "ocudu/fapi/p7/p7_indications_notifier.h"
#include "ocudu/fapi/p7/p7_slot_indication_notifier.h"
#include "ocudu/fapi_adaptor/mac/operation_controller.h"

using namespace ocudu;

namespace ocudu {

fapi_xsm_transport* g_l1_xsm_transport = nullptr;
} // namespace ocudu

namespace {


class xsm_operation_controller : public fapi_adaptor::operation_controller
{
public:
  void start() override {}
  void stop() override {}
};



class mac_fapi_p5_sector_adaptor_xsm : public fapi_adaptor::mac_fapi_p5_sector_adaptor
{
public:
  mac_fapi_p5_sector_adaptor_xsm(xsm_context& xsm)
    : p5_responses_(xsm), error_notifier_(xsm) {}

  fapi::p5_responses_notifier&    get_p5_responses_notifier() override    { return p5_responses_; }
  fapi::error_indication_notifier& get_error_indication_notifier() override { return error_notifier_; }
  fapi_adaptor::operation_controller& get_operation_controller() override  { return op_ctrl_; }

private:
  xsm_p5_responses_notifier      p5_responses_;
  xsm_error_indication_notifier  error_notifier_;
  xsm_operation_controller       op_ctrl_;
};



class mac_fapi_p7_sector_adaptor_xsm : public fapi_adaptor::mac_fapi_p7_sector_adaptor
{
public:
  mac_fapi_p7_sector_adaptor_xsm(xsm_context& xsm)
    : p7_indications_(xsm), p7_slot_(xsm), error_notifier_(xsm) {}

  fapi::p7_indications_notifier&     get_p7_indications_notifier() override     { return p7_indications_; }
  fapi::p7_slot_indication_notifier& get_p7_slot_indication_notifier() override { return p7_slot_; }
  fapi::error_indication_notifier&   get_error_indication_notifier() override   { return error_notifier_; }

private:
  xsm_p7_indications_notifier      p7_indications_;
  xsm_p7_slot_indication_notifier  p7_slot_;
  xsm_error_indication_notifier    error_notifier_;
};



class mac_fapi_p7_sector_adaptor_factory_xsm : public fapi_adaptor::mac_fapi_p7_sector_adaptor_factory
{
public:
  explicit mac_fapi_p7_sector_adaptor_factory_xsm(xsm_context& xsm) : xsm_(xsm) {}

  std::unique_ptr<fapi_adaptor::mac_fapi_p7_sector_adaptor>
  create(const fapi::cell_configuration& /*fapi_cfg*/,
         fapi::p7_requests_gateway&      p7_gateway,
         fapi::p7_last_request_notifier& p7_last_req_notifier,
         ru_controller&                  /*ru_ctrl*/) override
  {

    if (ocudu::g_l1_xsm_transport != nullptr) {
      ocudu::g_l1_xsm_transport->set_l1_p7_requests_gateway(&p7_gateway);
      ocudu::g_l1_xsm_transport->set_l1_p7_last_request_notifier(&p7_last_req_notifier);
    }
    return std::make_unique<mac_fapi_p7_sector_adaptor_xsm>(xsm_);
  }

private:
  xsm_context& xsm_;
};

} // namespace


std::unique_ptr<fapi_adaptor::mac_fapi_p5_sector_adaptor>
split6_o_du_low_plugin_xsm::create_fapi_p5_sector_adaptor(fapi::p5_requests_gateway& p5_gateway,
                                                          task_executor& /*executor*/,
                                                          task_executor& /*control_executor*/)
{
  if (g_l1_xsm_transport == nullptr) {
    return nullptr;
  }
  g_l1_xsm_transport->set_l1_p5_requests_gateway(&p5_gateway);
  return std::make_unique<mac_fapi_p5_sector_adaptor_xsm>(g_l1_xsm_transport->get_xsm_context());
}

std::unique_ptr<fapi_adaptor::mac_fapi_p7_sector_adaptor_factory>
split6_o_du_low_plugin_xsm::create_fapi_p7_sector_adaptor_factory(task_executor& /*executor*/,
                                                                  task_executor& /*control_executor*/)
{
  if (g_l1_xsm_transport == nullptr) {
    return nullptr;
  }
  return std::make_unique<mac_fapi_p7_sector_adaptor_factory_xsm>(g_l1_xsm_transport->get_xsm_context());
}
