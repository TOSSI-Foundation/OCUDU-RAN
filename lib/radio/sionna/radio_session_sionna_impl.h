

#pragma once

#include "radio_sionna_baseband_gateway.h"
#include "ocudu/channel/sionna/cir_source.h"
#include "ocudu/channel/sionna/cir_zmq_receiver.h"
#include "ocudu/radio/radio_session.h"
#include <memory>
#include <vector>

namespace ocudu {

class radio_session_sionna_impl : public radio_session
{
public:
  radio_session_sionna_impl(std::unique_ptr<radio_session>        transport_,
                            std::shared_ptr<cir_source>           source,
                            std::unique_ptr<cir_zmq_receiver>     receiver_,
                            const radio_configuration::radio&     config,
                            unsigned                              max_block_size,
                            unsigned                              max_taps);

  radio_management_plane& get_management_plane() override { return transport->get_management_plane(); }

  baseband_gateway& get_baseband_gateway(unsigned stream_id) override;

  baseband_gateway_timestamp read_current_time() override { return transport->read_current_time(); }

  void start(baseband_gateway_timestamp init_time) override { transport->start(init_time); }

  void stop() override { transport->stop(); }

private:
  std::unique_ptr<radio_session>                              transport;
  std::unique_ptr<cir_zmq_receiver>                           receiver;
  std::vector<std::unique_ptr<radio_sionna_baseband_gateway>> gateways;
};

} // namespace ocudu
