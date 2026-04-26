// SPDX-FileCopyrightText: Copyright (C) 2021-2026 Software Radio Systems Limited
// SPDX-License-Identifier: BSD-3-Clause-Open-MPI
// Portions of this file may implement 3GPP specifications, which may be subject to additional licensing requirements.

/// \file
/// \brief Metric decorator for the hardware-accelerated PUSCH decoder.

#pragma once

#include "ocudu/hal/phy/upper/channel_processors/pusch/hw_accelerator_pusch_dec.h"
#include "ocudu/phy/metrics/phy_metrics_notifiers.h"
#include "ocudu/phy/metrics/phy_metrics_reports.h"
#include "ocudu/support/ocudu_assert.h"
#include <chrono>
#include <memory>
#include <unordered_map>

namespace ocudu {

/// Hardware-accelerated PUSCH decoder metric decorator.
class phy_metrics_hwacc_pusch_dec_decorator : public hal::hw_accelerator_pusch_dec
{
public:
  phy_metrics_hwacc_pusch_dec_decorator(std::unique_ptr<hal::hw_accelerator_pusch_dec> base_,
                                        ldpc_decoder_metric_notifier&                  notifier_) :
    base(std::move(base_)), notifier(notifier_)
  {
    ocudu_assert(base, "Invalid PUSCH decoder HAL instance.");
  }

  void reserve_queue() override { base->reserve_queue(); }
  void free_queue() override { base->free_queue(); }
  void configure_operation(const hal::hw_pusch_decoder_configuration& config, unsigned cb_index = 0) override
  {
    base->configure_operation(config, cb_index);
  }
  void free_harq_context_entry(unsigned absolute_cb_id) override { base->free_harq_context_entry(absolute_cb_id); }
  bool is_harq_external() const override { return base->is_harq_external(); }

  bool enqueue_operation(span<const int8_t> data, span<const int8_t> aux_data = {}, unsigned cb_index = 0) override
  {
    const bool enq_ok = base->enqueue_operation(data, aux_data, cb_index);
    if (enq_ok) {
      pending& p      = states[cb_index];
      p.enqueue_tp    = std::chrono::high_resolution_clock::now();
      p.cb_sz_bits    = 0;
      p.duration      = std::chrono::nanoseconds{0};
      p.have_duration = false;
    }
    return enq_ok;
  }

  bool dequeue_operation(span<uint8_t> data, span<int8_t> aux_data = {}, unsigned segment_index = 0) override
  {
    const bool deq_ok = base->dequeue_operation(data, aux_data, segment_index);
    if (deq_ok) {
      auto it = states.find(segment_index);
      if (it != states.end()) {
        it->second.duration = std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::high_resolution_clock::now() - it->second.enqueue_tp);
        it->second.have_duration = true;
        it->second.cb_sz_bits    = data.size() * 8;
      }
    }
    return deq_ok;
  }

  void read_operation_outputs(hal::hw_pusch_decoder_outputs& out,
                              unsigned                       cb_index       = 0,
                              unsigned                       absolute_cb_id = 0) override
  {
    base->read_operation_outputs(out, cb_index, absolute_cb_id);
    auto it = states.find(cb_index);
    if (it != states.end() && it->second.have_duration) {
      ldpc_decoder_metrics m;
      m.cb_sz                 = units::bits(it->second.cb_sz_bits);
      m.nof_iterations        = out.nof_ldpc_iterations;
      m.crc_ok                = out.CRC_pass;
      m.measurements.duration = it->second.duration;
      notifier.on_new_metric(m);
      states.erase(it);
    }
  }

private:
  struct pending {
    std::chrono::high_resolution_clock::time_point enqueue_tp;
    std::chrono::nanoseconds                       duration;
    unsigned                                       cb_sz_bits    = 0;
    bool                                           have_duration = false;
  };

  std::unique_ptr<hal::hw_accelerator_pusch_dec> base;
  ldpc_decoder_metric_notifier&                  notifier;
  std::unordered_map<unsigned, pending>          states;
};

} // namespace ocudu
