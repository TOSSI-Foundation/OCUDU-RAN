
#pragma once

#include "ocudu/adt/complex.h"
#include "ocudu/adt/expected.h"
#include "ocudu/adt/span.h"
#include <string>
#include <vector>

namespace ocudu {

struct cir_artifact {
  double fs_hz = 0.0;
  double snapshot_dt_s = 0.0;
  /// Number of CIR snapshots in the timeline.
  unsigned nof_snapshots = 0;
  /// Number of transmit antennas.
  unsigned nof_tx_ant = 0;
  /// Number of receive antennas.
  unsigned nof_rx_ant = 0;
  /// Number of taps per link.
  unsigned nof_taps = 0;
  /// Restart the timeline when the last snapshot expires. Otherwise the last snapshot holds.
  bool loop = false;
  std::string normalization;
  std::string scene;
  std::vector<cf_t> taps;

  span<const cf_t> get_taps(unsigned snapshot, unsigned rx_ant, unsigned tx_ant) const
  {
    unsigned offset = ((snapshot * nof_rx_ant + rx_ant) * nof_tx_ant + tx_ant) * nof_taps;
    return span<const cf_t>(taps).subspan(offset, nof_taps);
  }
};


expected<cir_artifact, std::string> load_cir_artifact(const std::string& manifest_path);

} // namespace ocudu
