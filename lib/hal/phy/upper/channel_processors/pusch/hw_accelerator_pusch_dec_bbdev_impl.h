// SPDX-FileCopyrightText: Copyright (C) 2021-2026 Software Radio Systems Limited
// SPDX-License-Identifier: BSD-3-Clause-Open-MPI
// Portions of this file may implement 3GPP specifications, which may be subject to additional licensing requirements.

/// \file
/// \brief BBDEV-based implementation of the PUSCH decoder hardware accelerator.

#pragma once

#include "ocudu/adt/static_vector.h"
#include "ocudu/hal/dpdk/bbdev/bbdev_acc.h"
#include "ocudu/hal/dpdk/bbdev/bbdev_op_pool.h"
#include "ocudu/hal/dpdk/mbuf_pool.h"
#include "ocudu/hal/phy/upper/channel_processors/pusch/ext_harq_buffer_context_repository.h"
#include "ocudu/hal/phy/upper/channel_processors/pusch/hw_accelerator_pusch_dec.h"
#include <deque>
#include <unordered_map>
#include <vector>

namespace ocudu {
namespace hal {

/// BBDEV-backed hardware-accelerated PUSCH decoder.
class hw_accelerator_pusch_dec_bbdev_impl : public hw_accelerator_pusch_dec
{
public:
  hw_accelerator_pusch_dec_bbdev_impl(std::shared_ptr<dpdk::bbdev_acc>                    bbdev_accelerator_,
                                      std::shared_ptr<dpdk::mbuf_pool>                    input_pool_,
                                      std::shared_ptr<dpdk::mbuf_pool>                    hard_output_pool_,
                                      std::shared_ptr<dpdk::bbdev_op_pool>                op_pool_,
                                      std::shared_ptr<ext_harq_buffer_context_repository> harq_buffer_context_,
                                      bool                                                force_local_harq_,
                                      unsigned                                            decoder_id_,
                                      bool                                                dedicated_queue_);

  ~hw_accelerator_pusch_dec_bbdev_impl() override;

  void reserve_queue() override;
  void free_queue() override;
  void configure_operation(const hw_pusch_decoder_configuration& config, unsigned cb_index = 0) override;
  void read_operation_outputs(hw_pusch_decoder_outputs& out, unsigned cb_index = 0, unsigned absolute_cb_id = 0) override;
  void free_harq_context_entry(unsigned absolute_cb_id) override;
  bool is_harq_external() const override { return !force_local_harq; }

  bool enqueue_operation(span<const int8_t> data, span<const int8_t> aux_data = {}, unsigned cb_index = 0) override;
  bool dequeue_operation(span<uint8_t> data, span<int8_t> aux_data = {}, unsigned segment_index = 0) override;

private:
  struct cached_op_config {
    uint8_t            basegraph        = 1;
    uint16_t           z_c              = 0;
    uint8_t            q_m              = 2;
    uint16_t           n_filler         = 0;
    uint16_t           n_cb             = 0;
    uint8_t            rv               = 0;
    uint32_t           e                = 0;
    uint8_t            max_iter         = 8;
    bool               early_stop       = false;
    bool               new_data         = true;
    uint32_t           op_flags         = 0;
    unsigned           absolute_cb_id   = 0;
    unsigned           nof_segment_bits = 0;
    unsigned           nof_segments     = 1;
    unsigned           cb_crc_len       = 0;
    hw_dec_cb_crc_type crc_type         = hw_dec_cb_crc_type::CRC24B;
  };

  struct cached_op_output {
    bool     crc_pass   = false;
    unsigned iter_count = 0;
  };

  std::shared_ptr<dpdk::bbdev_acc>                    bbdev_accelerator;
  std::shared_ptr<dpdk::mbuf_pool>                    input_pool;
  std::shared_ptr<dpdk::mbuf_pool>                    hard_output_pool;
  std::shared_ptr<dpdk::bbdev_op_pool>                op_pool;
  std::shared_ptr<ext_harq_buffer_context_repository> harq_buffer_context;
  bool                                                force_local_harq;
  unsigned                                            decoder_id;
  bool                                                dedicated_queue;
  int                                                 queue_id = -1;
  std::unordered_map<unsigned, cached_op_config>      cfg_map;
  std::unordered_map<unsigned, cached_op_output>      out_map;
  std::vector<::rte_bbdev_dec_op*>                    pending_ops;
  std::deque<::rte_bbdev_dec_op*>                     completed_ops;
  unsigned                                            in_flight_count = 0;
  bool                                                e_limit_warned  = false;
  ocudulog::basic_logger&                             logger;

  void flush_pending();
  void poll_dequeue();
  void release_op(::rte_bbdev_dec_op* op);
};

} // namespace hal
} // namespace ocudu
