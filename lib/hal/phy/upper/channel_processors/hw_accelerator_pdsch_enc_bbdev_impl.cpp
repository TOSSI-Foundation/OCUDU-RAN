// SPDX-FileCopyrightText: Copyright (C) 2021-2026 Software Radio Systems Limited
// SPDX-License-Identifier: BSD-3-Clause-Open-MPI
// Portions of this file may implement 3GPP specifications, which may be subject to additional licensing requirements.

#include "hw_accelerator_pdsch_enc_bbdev_impl.h"
#include "ocudu/hal/dpdk/bbdev/bbdev_op_pool_factory.h"
#include "ocudu/hal/dpdk/mbuf_pool_factory.h"
#include "ocudu/ocudulog/ocudulog.h"
#include "ocudu/support/ocudu_assert.h"
#include <rte_bbdev.h>
#include <rte_bbdev_op.h>
#include <rte_lcore.h>
#include <rte_mbuf.h>
#include <rte_mempool.h>

using namespace ocudu;
using namespace hal;

namespace {

static uint8_t to_q_m(modulation_scheme mod)
{
  switch (mod) {
    case modulation_scheme::BPSK:
      return 1;
    case modulation_scheme::QPSK:
      return 2;
    case modulation_scheme::QAM16:
      return 4;
    case modulation_scheme::QAM64:
      return 6;
    case modulation_scheme::QAM256:
      return 8;
    default:
      return 2;
  }
}

static uint8_t to_basegraph(ldpc_base_graph_type bg)
{
  return (bg == ldpc_base_graph_type::BG1) ? 1 : 2;
}

static constexpr unsigned BITS_PER_BYTE = 8;

static constexpr unsigned BURST_SIZE = 64;

} // namespace

hw_accelerator_pdsch_enc_bbdev_impl::hw_accelerator_pdsch_enc_bbdev_impl(
    std::shared_ptr<dpdk::bbdev_acc>     bbdev_accelerator_,
    std::shared_ptr<dpdk::mbuf_pool>     input_pool_,
    std::shared_ptr<dpdk::mbuf_pool>     output_pool_,
    std::shared_ptr<dpdk::bbdev_op_pool> op_pool_,
    unsigned                             encoder_id_,
    bool                                 dedicated_queue_) :
  bbdev_accelerator(std::move(bbdev_accelerator_)),
  input_pool(std::move(input_pool_)),
  output_pool(std::move(output_pool_)),
  op_pool(std::move(op_pool_)),
  encoder_id(encoder_id_),
  dedicated_queue(dedicated_queue_),
  logger(bbdev_accelerator->get_logger())
{
  ocudu_assert(input_pool, "[bbdev_enc] null input mbuf pool passed to impl");
  ocudu_assert(output_pool, "[bbdev_enc] null output mbuf pool passed to impl");
  ocudu_assert(op_pool, "[bbdev_enc] null bbdev op pool passed to impl");
  if (dedicated_queue) {
    reserve_queue();
  }
}

hw_accelerator_pdsch_enc_bbdev_impl::~hw_accelerator_pdsch_enc_bbdev_impl()
{
  for (::rte_bbdev_enc_op* op : pending_ops) {
    release_op(op);
  }
  pending_ops.clear();
  while (in_flight_count > 0) {
    ::rte_bbdev_enc_op* batch[BURST_SIZE];
    uint16_t            n = ::rte_bbdev_dequeue_ldpc_enc_ops(
        bbdev_accelerator->get_device_id(), static_cast<uint16_t>(queue_id), batch, BURST_SIZE);
    if (n == 0) {
      break;
    }
    for (uint16_t i = 0; i < n; ++i) {
      completed_ops.push_back(batch[i]);
    }
    in_flight_count -= n;
  }
  for (::rte_bbdev_enc_op* op : completed_ops) {
    release_op(op);
  }
  completed_ops.clear();
  if (dedicated_queue && queue_id >= 0) {
    free_queue();
  }
}

void hw_accelerator_pdsch_enc_bbdev_impl::reserve_queue()
{
  if (queue_id < 0) {
    queue_id = bbdev_accelerator->reserve_queue(RTE_BBDEV_OP_LDPC_ENC);
  }
}

void hw_accelerator_pdsch_enc_bbdev_impl::free_queue()
{
  if (queue_id >= 0) {
    bbdev_accelerator->free_queue(RTE_BBDEV_OP_LDPC_ENC, static_cast<unsigned>(queue_id));
    queue_id = -1;
  }
}

void hw_accelerator_pdsch_enc_bbdev_impl::configure_operation(const hw_pdsch_encoder_configuration& config,
                                                              unsigned                              /*cb_index*/)
{
  current_cfg.basegraph          = to_basegraph(config.base_graph_index);
  current_cfg.z_c                = static_cast<uint16_t>(config.lifting_size);
  current_cfg.q_m                = to_q_m(config.modulation);
  current_cfg.n_filler           = static_cast<uint16_t>(config.nof_filler_bits);
  current_cfg.n_cb               = static_cast<uint16_t>(config.Ncb);
  current_cfg.rv                 = static_cast<uint8_t>(config.rv);
  current_cfg.e                  = config.rm_length;
  current_cfg.tb_crc_bits        = config.nof_tb_crc_bits;
  current_cfg.do_unpack          = config.do_unpack;
  current_cfg.cb_mode            = config.cb_mode;
  current_cfg.nof_segments       = static_cast<uint8_t>(config.nof_segments);
  current_cfg.nof_short_segments = static_cast<uint8_t>(config.nof_short_segments);
  current_cfg.cw_length_a        = config.cw_length_a;
  current_cfg.cw_length_b        = config.cw_length_b;

  current_cfg.op_flags = RTE_BBDEV_LDPC_RATE_MATCH;
  if (config.attach_cb_crc) {
    current_cfg.op_flags |= RTE_BBDEV_LDPC_CRC_24B_ATTACH;
  }
  if (config.attach_tb_crc) {
    current_cfg.op_flags |= RTE_BBDEV_LDPC_CRC_24A_ATTACH;
  }
}

bool hw_accelerator_pdsch_enc_bbdev_impl::enqueue_operation(span<const uint8_t> data,
                                                            span<const uint8_t> /*aux_data*/,
                                                            unsigned            /*cb_index*/)
{
  if (queue_id < 0) {
    logger.error("[bbdev_enc] enqueue before reserve_queue");
    return false;
  }

  static thread_local bool eal_thread_registered = false;
  if (!eal_thread_registered) {
    if (::rte_lcore_id() == LCORE_ID_ANY) {
      (void)::rte_thread_register();
    }
    eal_thread_registered = true;
  }

  ::rte_bbdev_enc_op* op = nullptr;
  if (::rte_bbdev_enc_op_alloc_bulk(op_pool->get_pool(), &op, 1) != 0 || op == nullptr) {
    logger.warning("[bbdev_enc] rte_bbdev_enc_op_alloc_bulk failed");
    return false;
  }

  ::rte_mbuf* m_in = ::rte_pktmbuf_alloc(input_pool->get_pool());
  if (m_in == nullptr) {
    logger.warning("[bbdev_enc] input mbuf alloc failed");
    ::rte_bbdev_enc_op_free_bulk(&op, 1);
    return false;
  }
  char* in_buf = ::rte_pktmbuf_append(m_in, data.size());
  if (in_buf == nullptr) {
    logger.warning("[bbdev_enc] input mbuf append failed (size {})", data.size());
    ::rte_pktmbuf_free(m_in);
    ::rte_bbdev_enc_op_free_bulk(&op, 1);
    return false;
  }
  std::memcpy(in_buf, data.data(), data.size());

  ::rte_mbuf* m_out = ::rte_pktmbuf_alloc(output_pool->get_pool());
  if (m_out == nullptr) {
    logger.warning("[bbdev_enc] output mbuf alloc failed");
    ::rte_pktmbuf_free(m_in);
    ::rte_bbdev_enc_op_free_bulk(&op, 1);
    return false;
  }

  // ACC100 requires K' (= K - n_filler) to be byte-aligned; round K' up to the next byte
  // and reduce n_filler by the padding delta.
  const unsigned K        = (current_cfg.basegraph == 1 ? 22U : 10U) * current_cfg.z_c;
  unsigned       n_filler = current_cfg.n_filler;
  const unsigned Kp_raw   = K - n_filler;
  const unsigned Kp_align = (Kp_raw + 7U) & ~7U;
  if (Kp_align > Kp_raw) {
    n_filler = K - Kp_align;
  }

  auto& enc         = op->ldpc_enc;
  enc.input.data    = m_in;
  enc.input.offset  = 0;
  enc.input.length  = data.size();
  enc.output.data   = m_out;
  enc.output.offset = 0;
  enc.output.length = 0;
  enc.basegraph     = current_cfg.basegraph;
  enc.z_c           = current_cfg.z_c;
  enc.q_m           = current_cfg.q_m;
  enc.n_filler      = static_cast<uint16_t>(n_filler);
  enc.n_cb          = current_cfg.n_cb;
  enc.rv_index      = current_cfg.rv;
  enc.op_flags      = current_cfg.op_flags;

  // Per DPDK BBDEV spec: a single-CB TB must be enqueued in TB-mode (c=1, cab=1).
  const bool single_cb_special = (current_cfg.nof_segments == 1);
  if (!single_cb_special) {
    enc.code_block_mode = RTE_BBDEV_CODE_BLOCK;
    enc.cb_params.e     = current_cfg.e;
  } else {
    enc.code_block_mode = RTE_BBDEV_TRANSPORT_BLOCK;
    enc.tb_params.c     = 1;
    enc.tb_params.r     = 0;
    enc.tb_params.cab   = 1;
    enc.tb_params.ea    = current_cfg.e;
    enc.tb_params.eb    = current_cfg.e;
  }

  pending_ops.push_back(op);

  if (pending_ops.size() >= BURST_SIZE) {
    flush_pending();
  }
  return true;
}

void hw_accelerator_pdsch_enc_bbdev_impl::flush_pending()
{
  if (pending_ops.empty() || queue_id < 0) {
    return;
  }
  const uint16_t n = ::rte_bbdev_enqueue_ldpc_enc_ops(bbdev_accelerator->get_device_id(),
                                                      static_cast<uint16_t>(queue_id),
                                                      pending_ops.data(),
                                                      static_cast<uint16_t>(pending_ops.size()));
  if (n > 0) {
    in_flight_count += n;
    pending_ops.erase(pending_ops.begin(), pending_ops.begin() + n);
  }
}

void hw_accelerator_pdsch_enc_bbdev_impl::poll_dequeue()
{
  if (in_flight_count == 0) {
    return;
  }
  ::rte_bbdev_enc_op* batch[BURST_SIZE];
  const uint16_t      n = ::rte_bbdev_dequeue_ldpc_enc_ops(bbdev_accelerator->get_device_id(),
                                                           static_cast<uint16_t>(queue_id),
                                                           batch,
                                                           BURST_SIZE);
  for (uint16_t i = 0; i < n; ++i) {
    completed_ops.push_back(batch[i]);
  }
  in_flight_count -= n;
}

void hw_accelerator_pdsch_enc_bbdev_impl::release_op(::rte_bbdev_enc_op* op)
{
  if (op == nullptr) {
    return;
  }
  if (op->ldpc_enc.input.data != nullptr) {
    ::rte_pktmbuf_free(op->ldpc_enc.input.data);
  }
  if (op->ldpc_enc.output.data != nullptr) {
    ::rte_pktmbuf_free(op->ldpc_enc.output.data);
  }
  ::rte_bbdev_enc_op_free_bulk(&op, 1);
}

bool hw_accelerator_pdsch_enc_bbdev_impl::dequeue_operation(span<uint8_t> data,
                                                            span<uint8_t> /*aux_data*/,
                                                            unsigned      /*segment_index*/)
{
  if (completed_ops.empty() && pending_ops.empty() && in_flight_count == 0) {
    logger.warning("[bbdev_enc] dequeue called with no ops outstanding");
    return false;
  }

  constexpr unsigned MAX_ITERS = 1'000'000;
  unsigned           iters     = 0;
  while (completed_ops.empty()) {
    flush_pending();
    poll_dequeue();
    if (++iters > MAX_ITERS) {
      logger.warning("[bbdev_enc] dequeue timeout (pending={}, in_flight={})",
                     pending_ops.size(),
                     in_flight_count);
      return false;
    }
  }

  ::rte_bbdev_enc_op* deq_op = completed_ops.front();
  completed_ops.pop_front();

  bool ok = true;
  if (deq_op->status != 0) {
    logger.warning("[bbdev_enc] op status={}", deq_op->status);
    ok = false;
  } else {
    ::rte_mbuf*    m_out   = deq_op->ldpc_enc.output.data;
    const unsigned length  = deq_op->ldpc_enc.output.length;
    const char*    src     = rte_pktmbuf_mtod(m_out, const char*);
    const size_t   to_copy = std::min(static_cast<size_t>(length), data.size());
    std::memcpy(data.data(), src, to_copy);
  }

  release_op(deq_op);
  return ok;
}
