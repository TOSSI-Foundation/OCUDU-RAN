
#include "radio_sionna_baseband_gateway.h"
#include "ocudu/ocuduvec/conversion.h"
#include "ocudu/support/error_handling.h"
#include <limits>

using namespace ocudu;

static constexpr float ci16_cf_scaling = std::numeric_limits<int16_t>::max();

radio_sionna_baseband_gateway::radio_sionna_baseband_gateway(baseband_gateway&           gateway_,
                                                             std::shared_ptr<cir_source> source,
                                                             unsigned                    nof_tx_channels,
                                                             unsigned                    max_block_size,
                                                             unsigned                    max_taps,
                                                             ocudulog::basic_logger&     logger_) :
  gateway(gateway_),
  logger(logger_),
  engine(std::move(source), nof_tx_channels, max_block_size, max_taps),
  transmitter(*this),
  tx_cf(max_block_size),
  max_block(max_block_size)
{
  tx_ci16.resize(nof_tx_channels);
  for (std::vector<ci16_t>& buffer : tx_ci16) {
    buffer.resize(max_block_size);
  }
}

void radio_sionna_baseband_gateway::transmitter_adaptor::transmit(
    const baseband_gateway_buffer_reader&        data,
    const baseband_gateway_transmitter_metadata& metadata)
{
  unsigned nof_channels = data.get_nof_channels();
  unsigned nof_samples  = data.get_nof_samples();

  // Pass through unfiltered rather than drop the transmission if the block exceeds capacity.
  if ((nof_samples > parent.max_block) || (nof_channels > parent.tx_ci16.size())) {
    if (!parent.overflow_reported) {
      parent.logger.warning(
          "Sionna channel bypassed: block of {} samples on {} channels exceeds the configured capacity ({} samples, {} "
          "channels).",
          nof_samples,
          nof_channels,
          parent.max_block,
          parent.tx_ci16.size());
      parent.overflow_reported = true;
    }
    parent.gateway.get_transmitter().transmit(data, metadata);
    return;
  }

  for (unsigned channel_idx = 0; channel_idx != nof_channels; ++channel_idx) {
    span<cf_t> samples = span<cf_t>(parent.tx_cf).first(nof_samples);
    ocuduvec::convert(samples, data.get_channel_buffer(channel_idx), ci16_cf_scaling);

    parent.engine.process(channel_idx, samples, metadata.ts);

    span<ci16_t> out = span<ci16_t>(parent.tx_ci16[channel_idx]).first(nof_samples);
    ocuduvec::convert(out, samples, ci16_cf_scaling);
  }

  filtered_buffer_reader reader(parent.tx_ci16, nof_samples);
  parent.gateway.get_transmitter().transmit(reader, metadata);
}
