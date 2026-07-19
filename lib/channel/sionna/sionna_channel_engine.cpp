#include "ocudu/channel/sionna/sionna_channel_engine.h"
#include "ocudu/ocuduvec/add.h"
#include "ocudu/ocuduvec/copy.h"
#include "ocudu/ocuduvec/sc_prod.h"
#include "ocudu/ocuduvec/zero.h"
#include "ocudu/support/error_handling.h"
#include "ocudu/support/ocudu_assert.h"
#include <algorithm>
#include <cmath>

using namespace ocudu;

static uint64_t compute_snapshot_len(const cir_artifact& artifact)
{
  if (artifact.nof_snapshots <= 1) {
    return 0;
  }
  uint64_t len = static_cast<uint64_t>(std::llround(artifact.snapshot_dt_s * artifact.fs_hz));
  return std::max<uint64_t>(len, 1);
}

sionna_channel_engine::sionna_channel_engine(std::shared_ptr<cir_source> source_,
                                             unsigned                    nof_channels,
                                             unsigned                    max_block_size,
                                             unsigned                    max_taps_) :
  source(std::move(source_)), max_block(max_block_size), max_taps(max_taps_)
{
  report_fatal_error_if_not(source != nullptr, "CIR source must not be null.");
  report_fatal_error_if_not(nof_channels != 0, "Number of channels must not be zero.");
  report_fatal_error_if_not(max_block != 0, "Maximum block size must not be zero.");
  report_fatal_error_if_not(max_taps != 0, "Maximum number of taps must not be zero.");

  artifact = source->get_artifact();
  if (artifact) {
    report_fatal_error_if_not(artifact->nof_taps <= max_taps,
                              "Artifact declares {} taps, exceeding the configured maximum {}.",
                              artifact->nof_taps,
                              max_taps);
    snapshot_len_samples = compute_snapshot_len(*artifact);
  }

  unsigned hist_len = max_taps - 1;
  states.resize(nof_channels);
  for (channel_state& state : states) {
    state.history.assign(hist_len, cf_t());
  }
  ext_buffer.resize(hist_len + max_block);
  acc_buffer.resize(max_block);
  tap_buffer.resize(max_block);
}

unsigned sionna_channel_engine::get_snapshot_index(const cir_artifact&        artifact,
                                                   uint64_t                   snapshot_len_samples,
                                                   baseband_gateway_timestamp timestamp)
{
  if ((snapshot_len_samples == 0) || (artifact.nof_snapshots <= 1)) {
    return 0;
  }
  uint64_t index = timestamp / snapshot_len_samples;
  if (artifact.loop) {
    return static_cast<unsigned>(index % artifact.nof_snapshots);
  }
  return static_cast<unsigned>(std::min<uint64_t>(index, artifact.nof_snapshots - 1));
}

void sionna_channel_engine::reset()
{
  for (channel_state& state : states) {
    std::fill(state.history.begin(), state.history.end(), cf_t());
    state.synced = false;
  }
}

void sionna_channel_engine::process(unsigned channel_idx, span<cf_t> inout, baseband_gateway_timestamp timestamp)
{
  ocudu_assert(channel_idx < states.size(), "Invalid channel index {}.", channel_idx);
  ocudu_assert(inout.size() <= max_block, "Block size {} exceeds maximum {}.", inout.size(), max_block);

  unsigned nof_samples = inout.size();
  if (nof_samples == 0) {
    return;
  }

  // Swap only between blocks, so each block is filtered with a consistent tap set.
  std::shared_ptr<const cir_artifact> published = source->get_artifact();
  if (published != artifact) {
    if (published && (published->nof_taps > max_taps)) {
      // Keep the previous channel rather than overrunning the preallocated history.
      published = artifact;
    } else {
      artifact             = std::move(published);
      snapshot_len_samples = artifact ? compute_snapshot_len(*artifact) : 0;
      ++artifact_changes;
    }
  }

  if (!artifact) {
    return;
  }

  channel_state& state    = states[channel_idx];
  unsigned       nof_taps = artifact->nof_taps;
  unsigned       hist_len = nof_taps - 1;

  // Model timestamp gaps as zero signal shifted through the filter history.
  if (state.synced && (timestamp != state.next_timestamp) && (hist_len != 0)) {
    span<cf_t> hist = span<cf_t>(state.history).first(hist_len);
    if (timestamp > state.next_timestamp) {
      uint64_t gap = timestamp - state.next_timestamp;
      if (gap >= hist_len) {
        std::fill(hist.begin(), hist.end(), cf_t());
      } else {
        unsigned shift = static_cast<unsigned>(gap);
        std::move(hist.begin() + shift, hist.end(), hist.begin());
        std::fill(hist.end() - shift, hist.end(), cf_t());
      }
    } else {
      std::fill(hist.begin(), hist.end(), cf_t());
    }
  }
  state.next_timestamp = timestamp + nof_samples;
  state.synced         = true;

  unsigned tx_ant = std::min(channel_idx, artifact->nof_tx_ant - 1);
  unsigned rx_ant = std::min(channel_idx, artifact->nof_rx_ant - 1);
  span<const cf_t> taps =
      artifact->get_taps(get_snapshot_index(*artifact, snapshot_len_samples, timestamp), rx_ant, tx_ant);

  span<cf_t> ext = span<cf_t>(ext_buffer).first(hist_len + nof_samples);
  ocuduvec::copy(ext.first(hist_len), span<const cf_t>(state.history).first(hist_len));
  ocuduvec::copy(ext.subspan(hist_len, nof_samples), inout);

  span<cf_t> acc = span<cf_t>(acc_buffer).first(nof_samples);
  ocuduvec::zero(acc);

  span<cf_t> scratch = span<cf_t>(tap_buffer).first(nof_samples);
  for (unsigned k = 0; k != nof_taps; ++k) {
    cf_t tap = taps[k];
    if ((tap.real() == 0.0F) && (tap.imag() == 0.0F)) {
      continue;
    }
    span<const cf_t> src = span<const cf_t>(ext).subspan(hist_len - k, nof_samples);
    ocuduvec::sc_prod(scratch, src, tap);
    ocuduvec::add(acc, acc, scratch);
  }

  if (hist_len != 0) {
    ocuduvec::copy(span<cf_t>(state.history).first(hist_len), ext.last(hist_len));
  }
  ocuduvec::copy(inout, acc);
}
