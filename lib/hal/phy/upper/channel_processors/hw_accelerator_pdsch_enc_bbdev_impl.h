// SPDX-FileCopyrightText: Copyright (C) 2021-2026 Software Radio Systems Limited
// SPDX-License-Identifier: BSD-3-Clause-Open-MPI
// Portions of this file may implement 3GPP specifications, which may be subject to additional licensing requirements.

/// \file
/// \brief BBDEV-based implementation of the PDSCH encoder hardware accelerator.

#pragma once

#include "ocudu/hal/dpdk/bbdev/bbdev_acc.h"
#include "ocudu/hal/dpdk/bbdev/bbdev_op_pool.h"
#include "ocudu/hal/dpdk/mbuf_pool.h"
#include "ocudu/hal/phy/upper/channel_processors/hw_accelerator_pdsch_enc.h"
#include <deque>
#include <vector>

namespace ocudu {
namespace hal {

/// BBDEV-backed hardware-accelerated PDSCH encoder.
class hw_accelerator_pdsch_enc_bbdev_impl : public hw_accelerator_pdsch_enc
{
public:
  hw_accelerator_pdsch_enc_bbdev_impl(std::shared_ptr<dpdk::bbdev_acc>     bbdev_accelerator_,
                                      std::shared_ptr<dpdk::mbuf_pool>     input_pool_,
                                      std::shared_ptr<dpdk::mbuf_pool>     output_pool_,
                                      std::shared_ptr<dpdk::bbdev_op_pool> op_pool_,
                                      unsigned                             encoder_id_,
                                      bool                                 dedicated_queue_);

  ~hw_accelerator_pdsch_enc_bbdev_impl() override;

  void     reserve_queue() override;
  void     free_queue() override;
  void     configure_operation(const hw_pdsch_encoder_configuration& config, unsigned cb_index = 0) override;
  bool     is_cb_mode_supported() const override { return true; }
  unsigned get_max_supported_buff_size() const override { return RTE_BBDEV_LDPC_E_MAX_MBUF; }

  bool enqueue_operation(span<const uint8_t> data, span<const uint8_t> aux_data = {}, unsigned cb_index = 0) override;
  bool dequeue_operation(span<uint8_t> data, span<uint8_t> aux_data = {}, unsigned segment_index = 0) override;

private:
  struct cached_op_config {
    uint8_t  basegraph          = 1;
    uint16_t z_c                = 0;
    uint8_t  q_m                = 2;
    uint16_t n_filler           = 0;
    uint16_t n_cb               = 0;
    uint8_t  rv                 = 0;
    uint32_t op_flags           = 0;
    unsigned tb_crc_bits        = 0;
    bool     do_unpack          = true;
    uint32_t e                  = 0;
    bool     cb_mode            = true;
    uint8_t  nof_segments       = 0;
    uint8_t  nof_short_segments = 0;
    uint32_t cw_length_a        = 0;
    uint32_t cw_length_b        = 0;
  };

  void flush_pending();
  void poll_dequeue();
  void release_op(::rte_bbdev_enc_op* op);

  std::shared_ptr<dpdk::bbdev_acc>     bbdev_accelerator;
  std::shared_ptr<dpdk::mbuf_pool>     input_pool;
  std::shared_ptr<dpdk::mbuf_pool>     output_pool;
  std::shared_ptr<dpdk::bbdev_op_pool> op_pool;
  unsigned                             encoder_id;
  bool                                 dedicated_queue;
  int                                  queue_id = -1;
  cached_op_config                     current_cfg;
  std::vector<::rte_bbdev_enc_op*>     pending_ops;
  std::deque<::rte_bbdev_enc_op*>      completed_ops;
  unsigned                             in_flight_count = 0;
  ocudulog::basic_logger&              logger;
};

} // namespace hal
} // namespace ocudu
