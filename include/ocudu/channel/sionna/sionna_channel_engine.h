#pragma once

#include "ocudu/adt/complex.h"
#include "ocudu/adt/span.h"
#include "ocudu/channel/sionna/cir_artifact.h"
#include "ocudu/channel/sionna/cir_source.h"
#include "ocudu/gateways/baseband/baseband_gateway_timestamp.h"
#include <memory>
#include <vector>

namespace ocudu {

class sionna_channel_engine
{
public:
  sionna_channel_engine(std::shared_ptr<cir_source> source_,
                        unsigned                    nof_channels,
                        unsigned                    max_block_size,
                        unsigned                    max_taps);


  void process(unsigned channel_idx, span<cf_t> inout, baseband_gateway_timestamp timestamp);

  void reset();

  static unsigned
  get_snapshot_index(const cir_artifact& artifact, uint64_t snapshot_len_samples, baseband_gateway_timestamp timestamp);

  unsigned get_snapshot_index(baseband_gateway_timestamp timestamp) const
  {
    return artifact ? get_snapshot_index(*artifact, snapshot_len_samples, timestamp) : 0;
  }

  uint64_t get_nof_artifact_changes() const { return artifact_changes; }

private:
  struct channel_state {
    std::vector<cf_t>          history;
    baseband_gateway_timestamp next_timestamp = 0;
    bool                       synced         = false;
  };

  std::shared_ptr<cir_source>         source;
  std::shared_ptr<const cir_artifact> artifact;
  uint64_t                            snapshot_len_samples = 0;
  std::vector<channel_state>          states;
  std::vector<cf_t>                   ext_buffer;
  std::vector<cf_t>                   acc_buffer;
  std::vector<cf_t>                   tap_buffer;
  unsigned                            max_block;
  unsigned                            max_taps;
  uint64_t                            artifact_changes = 0;
};

} // namespace ocudu
