

#pragma once

#include "ocudu/channel/sionna/cir_source.h"
#include "ocudu/channel/sionna/sionna_channel_engine.h"
#include "ocudu/gateways/baseband/baseband_gateway.h"
#include "ocudu/gateways/baseband/baseband_gateway_receiver.h"
#include "ocudu/gateways/baseband/baseband_gateway_transmitter.h"
#include "ocudu/gateways/baseband/buffer/baseband_gateway_buffer_reader.h"
#include "ocudu/gateways/baseband/buffer/baseband_gateway_buffer_writer.h"
#include "ocudu/ocudulog/ocudulog.h"
#include <memory>
#include <vector>

namespace ocudu {

class radio_sionna_baseband_gateway : public baseband_gateway
{
public:
  radio_sionna_baseband_gateway(baseband_gateway&           gateway_,
                                std::shared_ptr<cir_source> source,
                                unsigned                    nof_tx_channels,
                                unsigned                    max_block_size,
                                unsigned                    max_taps,
                                ocudulog::basic_logger&     logger_);

  baseband_gateway_transmitter& get_transmitter() override { return transmitter; }

  baseband_gateway_receiver& get_receiver() override { return gateway.get_receiver(); }

  unsigned get_transmitter_optimal_buffer_size() const override
  {
    return gateway.get_transmitter_optimal_buffer_size();
  }

  unsigned get_receiver_optimal_buffer_size() const override { return gateway.get_receiver_optimal_buffer_size(); }

private:
  class transmitter_adaptor : public baseband_gateway_transmitter
  {
  public:
    transmitter_adaptor(radio_sionna_baseband_gateway& parent_) : parent(parent_) {}

    void transmit(const baseband_gateway_buffer_reader&        data,
                  const baseband_gateway_transmitter_metadata& metadata) override;

  private:
    radio_sionna_baseband_gateway& parent;
  };

  class filtered_buffer_reader : public baseband_gateway_buffer_reader
  {
  public:
    filtered_buffer_reader(const std::vector<std::vector<ci16_t>>& data_, unsigned nof_samples_) :
      data(data_), nof_samples(nof_samples_)
    {
    }

    unsigned get_nof_channels() const override { return data.size(); }

    unsigned get_nof_samples() const override { return nof_samples; }

    span<const ci16_t> get_channel_buffer(unsigned channel_idx) const override
    {
      return span<const ci16_t>(data[channel_idx]).first(nof_samples);
    }

  private:
    const std::vector<std::vector<ci16_t>>& data;
    unsigned                                nof_samples;
  };

  baseband_gateway&                     gateway;
  ocudulog::basic_logger&               logger;
  sionna_channel_engine                 engine;
  transmitter_adaptor                   transmitter;
  std::vector<std::vector<ci16_t>>      tx_ci16;
  std::vector<cf_t>                     tx_cf;
  unsigned                              max_block;
  bool                                  overflow_reported = false;
};

} // namespace ocudu
