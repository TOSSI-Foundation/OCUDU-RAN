

#include "radio_session_sionna_impl.h"
#include "ocudu/ocudulog/ocudulog.h"
#include "ocudu/support/error_handling.h"
#include "ocudu/support/ocudu_assert.h"

using namespace ocudu;

radio_session_sionna_impl::radio_session_sionna_impl(std::unique_ptr<radio_session>    transport_,
                                                     std::shared_ptr<cir_source>       source,
                                                     std::unique_ptr<cir_zmq_receiver> receiver_,
                                                     const radio_configuration::radio& config,
                                                     unsigned                          max_block_size,
                                                     unsigned                          max_taps) :
  transport(std::move(transport_)), receiver(std::move(receiver_))
{
  report_fatal_error_if_not(transport != nullptr, "Transport radio session must not be null.");

  ocudulog::basic_logger& logger = ocudulog::fetch_basic_logger("RF", false);

  gateways.reserve(config.tx_streams.size());
  for (unsigned stream_id = 0, nof_streams = config.tx_streams.size(); stream_id != nof_streams; ++stream_id) {
    gateways.emplace_back(
        std::make_unique<radio_sionna_baseband_gateway>(transport->get_baseband_gateway(stream_id),
                                                        source,
                                                        config.tx_streams[stream_id].channels.size(),
                                                        max_block_size,
                                                        max_taps,
                                                        logger));
  }
}

baseband_gateway& radio_session_sionna_impl::get_baseband_gateway(unsigned stream_id)
{
  ocudu_assert(stream_id < gateways.size(),
               "Stream identifier (i.e., {}) exceeds the number of baseband gateways (i.e., {})",
               stream_id,
               gateways.size());
  return *gateways[stream_id];
}
