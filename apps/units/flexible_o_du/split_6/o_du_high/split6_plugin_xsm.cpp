#include "split6_plugin_xsm.h"

#include "apps/du/fapi_xsm_proxy.h"
#include "apps/du/fapi_xsm_transport.h"
#include "apps/units/flexible_o_du/o_du_unit.h"
#include "ocudu/du/du_high/du_high_configuration.h"
#include "ocudu/fapi_adaptor/phy/p5/phy_fapi_p5_sector_adaptor.h"
#include "ocudu/fapi_adaptor/phy/p7/phy_fapi_p7_sector_adaptor.h"
#include "ocudu/fapi_adaptor/phy/phy_fapi_adaptor.h"

#include <cstdio>

using namespace ocudu;

namespace {


class fapi_adaptor_xsm : public fapi_adaptor::phy_fapi_adaptor,
                         public fapi_adaptor::phy_fapi_sector_adaptor,
                         public fapi_adaptor::phy_fapi_p7_sector_adaptor,
                         public fapi_adaptor::phy_fapi_p5_sector_adaptor
{
public:
  explicit fapi_adaptor_xsm(fapi_xsm_transport& transport) :
    transport_(transport),
    p5_gateway_(transport.get_xsm_context()),
    p7_gateway_(transport.get_xsm_context()),
    p7_last_req_notifier_(transport.get_xsm_context())
  {
  }

  fapi_adaptor::phy_fapi_sector_adaptor& get_sector_adaptor(unsigned /*cell_id*/) override { return *this; }

  fapi_adaptor::phy_fapi_p5_sector_adaptor& get_p5_sector_adaptor() override { return *this; }
  fapi_adaptor::phy_fapi_p7_sector_adaptor& get_p7_sector_adaptor() override { return *this; }
  void start() override {}
  void stop() override {}

  fapi::p5_requests_gateway& get_p5_requests_gateway() override { return p5_gateway_; }

  void set_p5_responses_notifier(fapi::p5_responses_notifier& notifier) override
  {
    transport_.set_l2_p5_responses_notifier(&notifier);
  }

  void set_error_indication_notifier(fapi::error_indication_notifier& notifier) override
  {
    transport_.set_l2_error_notifier(&notifier);
  }

  fapi::p7_requests_gateway&      get_p7_requests_gateway() override      { return p7_gateway_; }
  fapi::p7_last_request_notifier& get_p7_last_request_notifier() override { return p7_last_req_notifier_; }

  void set_p7_slot_indication_notifier(fapi::p7_slot_indication_notifier& notifier) override
  {
    transport_.set_l2_p7_slot_notifier(&notifier);
  }

  void set_p7_indications_notifier(fapi::p7_indications_notifier& notifier) override
  {
    transport_.set_l2_p7_indications_notifier(&notifier);
  }

private:
  fapi_xsm_transport&           transport_;
  xsm_p5_requests_gateway       p5_gateway_;
  xsm_p7_requests_gateway       p7_gateway_;
  xsm_p7_last_request_notifier  p7_last_req_notifier_;
};

} // namespace

std::unique_ptr<fapi_adaptor::phy_fapi_adaptor>
split6_plugin_xsm::create_fapi_adaptor(const odu::du_high_configuration& /*du_high_cfg*/,
                                       const o_du_unit_dependencies&     dependencies)
{
  if (dependencies.xsm_transport == nullptr) {
    return nullptr;
  }
  return std::make_unique<fapi_adaptor_xsm>(*dependencies.xsm_transport);
}
