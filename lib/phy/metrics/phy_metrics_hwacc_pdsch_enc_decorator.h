// SPDX-FileCopyrightText: Copyright (C) 2021-2026 Software Radio Systems Limited
// SPDX-License-Identifier: BSD-3-Clause-Open-MPI
// Portions of this file may implement 3GPP specifications, which may be subject to additional licensing requirements.

/// \file
/// \brief Metric decorator for the hardware-accelerated PDSCH encoder.

#pragma once

#include "ocudu/hal/phy/upper/channel_processors/hw_accelerator_pdsch_enc.h"
#include "ocudu/phy/metrics/phy_metrics_notifiers.h"
#include "ocudu/phy/metrics/phy_metrics_reports.h"
#include "ocudu/support/ocudu_assert.h"
#include <chrono>
#include <memory>
#include <unordered_map>

namespace ocudu {

/// Hardware-accelerated PDSCH encoder metric decorator.
class phy_metrics_hwacc_pdsch_enc_decorator : public hal::hw_accelerator_pdsch_enc
{
public:
  phy_metrics_hwacc_pdsch_enc_decorator(std::unique_ptr<hal::hw_accelerator_pdsch_enc> base_,
                                        ldpc_encoder_metric_notifier&                  notifier_) :
    base(std::move(base_)), notifier(notifier_)
  {
    ocudu_assert(base, "Invalid PDSCH encoder HAL instance.");
  }

  void reserve_queue() override { base->reserve_queue(); }
  void free_queue() override { base->free_queue(); }
  void configure_operation(const hal::hw_pdsch_encoder_configuration& config, unsigned cb_index = 0) override
  {
    base->configure_operation(config, cb_index);
  }
  bool     is_cb_mode_supported() const override { return base->is_cb_mode_supported(); }
  unsigned get_max_supported_buff_size() const override { return base->get_max_supported_buff_size(); }

  bool enqueue_operation(span<const uint8_t> data, span<const uint8_t> aux_data = {}, unsigned cb_index = 0) override
  {
    const bool enq_ok = base->enqueue_operation(data, aux_data, cb_index);
    if (enq_ok) {
      pending& p   = states[cb_index];
      p.enqueue_tp = std::chrono::high_resolution_clock::now();
      p.cb_sz_bits = data.size() * 8;
    }
    return enq_ok;
  }

  bool dequeue_operation(span<uint8_t> data, span<uint8_t> aux_data = {}, unsigned segment_index = 0) override
  {
    const bool deq_ok = base->dequeue_operation(data, aux_data, segment_index);
    if (deq_ok) {
      auto it = states.find(segment_index);
      if (it != states.end()) {
        ldpc_encoder_metrics m;
        m.cb_sz                 = units::bits(it->second.cb_sz_bits);
        m.measurements.duration = std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::high_resolution_clock::now() - it->second.enqueue_tp);
        notifier.on_new_metric(m);
        states.erase(it);
      }
    }
    return deq_ok;
  }

private:
  struct pending {
    std::chrono::high_resolution_clock::time_point enqueue_tp;
    unsigned                                       cb_sz_bits = 0;
  };

  std::unique_ptr<hal::hw_accelerator_pdsch_enc> base;
  ldpc_encoder_metric_notifier&                  notifier;
  std::unordered_map<unsigned, pending>          states;
};

} // namespace ocudu
