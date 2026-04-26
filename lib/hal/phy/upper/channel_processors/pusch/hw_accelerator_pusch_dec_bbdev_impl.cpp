// SPDX-FileCopyrightText: Copyright (C) 2021-2026 Software Radio Systems Limited
// SPDX-License-Identifier: BSD-3-Clause-Open-MPI
// Portions of this file may implement 3GPP specifications, which may be subject to additional licensing requirements.

#include "hw_accelerator_pusch_dec_bbdev_impl.h"
#include "ocudu/hal/dpdk/bbdev/bbdev_op_pool_factory.h"
#include "ocudu/hal/dpdk/mbuf_pool_factory.h"
#include "ocudu/ocudulog/ocudulog.h"
#include "ocudu/support/ocudu_assert.h"
#include <cstring>
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

static constexpr unsigned BURST_SIZE = 64;

} // namespace

hw_accelerator_pusch_dec_bbdev_impl::hw_accelerator_pusch_dec_bbdev_impl(
    std::shared_ptr<dpdk::bbdev_acc>                    bbdev_accelerator_,
    std::shared_ptr<dpdk::mbuf_pool>                    input_pool_,
    std::shared_ptr<dpdk::mbuf_pool>                    hard_output_pool_,
    std::shared_ptr<dpdk::bbdev_op_pool>                op_pool_,
    std::shared_ptr<ext_harq_buffer_context_repository> harq_buffer_context_,
    bool                                                force_local_harq_,
    unsigned                                            decoder_id_,
    bool                                                dedicated_queue_) :
  bbdev_accelerator(std::move(bbdev_accelerator_)),
  input_pool(std::move(input_pool_)),
  hard_output_pool(std::move(hard_output_pool_)),
  op_pool(std::move(op_pool_)),
  harq_buffer_context(std::move(harq_buffer_context_)),
  force_local_harq(force_local_harq_),
  decoder_id(decoder_id_),
  dedicated_queue(dedicated_queue_),
  logger(bbdev_accelerator->get_logger())
{
  ocudu_assert(input_pool, "[bbdev_dec] null input LLR mbuf pool passed to impl");
  ocudu_assert(hard_output_pool, "[bbdev_dec] null hard-output mbuf pool passed to impl");
  ocudu_assert(op_pool, "[bbdev_dec] null bbdev op pool passed to impl");
  if (dedicated_queue) {
    reserve_queue();
  }
}

hw_accelerator_pusch_dec_bbdev_impl::~hw_accelerator_pusch_dec_bbdev_impl()
{
  for (::rte_bbdev_dec_op* op : pending_ops) {
    release_op(op);
  }
  pending_ops.clear();
  while (in_flight_count > 0) {
    ::rte_bbdev_dec_op* batch[BURST_SIZE];
    uint16_t            n = ::rte_bbdev_dequeue_ldpc_dec_ops(
        bbdev_accelerator->get_device_id(), static_cast<uint16_t>(queue_id), batch, BURST_SIZE);
    if (n == 0) {
      break;
    }
    for (uint16_t i = 0; i < n; ++i) {
      completed_ops.push_back(batch[i]);
    }
    in_flight_count -= n;
  }
  for (::rte_bbdev_dec_op* op : completed_ops) {
    release_op(op);
  }
  completed_ops.clear();
  if (dedicated_queue && queue_id >= 0) {
    free_queue();
  }
}

void hw_accelerator_pusch_dec_bbdev_impl::reserve_queue()
{
  if (queue_id < 0) {
    queue_id = bbdev_accelerator->reserve_queue(RTE_BBDEV_OP_LDPC_DEC);
  }
}

void hw_accelerator_pusch_dec_bbdev_impl::free_queue()
{
  if (queue_id >= 0) {
    bbdev_accelerator->free_queue(RTE_BBDEV_OP_LDPC_DEC, static_cast<unsigned>(queue_id));
    queue_id = -1;
  }
}

void hw_accelerator_pusch_dec_bbdev_impl::configure_operation(const hw_pusch_decoder_configuration& config,
                                                              unsigned                              cb_index)
{
  cached_op_config cfg;
  cfg.basegraph        = to_basegraph(config.base_graph_index);
  cfg.z_c              = static_cast<uint16_t>(config.lifting_size);
  cfg.q_m              = to_q_m(config.modulation);
  cfg.n_filler         = static_cast<uint16_t>(config.nof_filler_bits);
  cfg.n_cb             = static_cast<uint16_t>(config.Ncb);
  cfg.rv               = static_cast<uint8_t>(config.rv);
  cfg.e                = config.cw_length;
  cfg.max_iter         = static_cast<uint8_t>(config.max_nof_ldpc_iterations);
  cfg.early_stop       = config.use_early_stop;
  cfg.new_data         = config.new_data;
  cfg.absolute_cb_id   = config.absolute_cb_id;
  cfg.nof_segment_bits = config.nof_segment_bits;
  cfg.nof_segments     = config.nof_segments;
  cfg.cb_crc_len       = config.cb_crc_len;
  cfg.crc_type         = config.cb_crc_type;

  uint32_t flags = 0;
  if (config.cb_crc_type == hw_dec_cb_crc_type::CRC24B) {
    flags |= RTE_BBDEV_LDPC_CRC_TYPE_24B_CHECK;
    flags |= RTE_BBDEV_LDPC_CRC_TYPE_24B_DROP;
  }
  if (!force_local_harq) {
    if (!config.new_data) {
      flags |= RTE_BBDEV_LDPC_HQ_COMBINE_IN_ENABLE;
    }
    flags |= RTE_BBDEV_LDPC_HQ_COMBINE_OUT_ENABLE;
    flags |= RTE_BBDEV_LDPC_INTERNAL_HARQ_MEMORY_IN_ENABLE;
    flags |= RTE_BBDEV_LDPC_INTERNAL_HARQ_MEMORY_OUT_ENABLE;
  }
  if (config.use_early_stop) {
    flags |= RTE_BBDEV_LDPC_ITERATION_STOP_ENABLE;
  }
  cfg.op_flags = flags;

  cfg_map[cb_index] = cfg;
}

bool hw_accelerator_pusch_dec_bbdev_impl::enqueue_operation(span<const int8_t> data,
                                                            span<const int8_t> /*aux_data*/,
                                                            unsigned           cb_index)
{
  if (queue_id < 0) {
    logger.error("[bbdev_dec] enqueue before reserve_queue");
    return false;
  }

  static thread_local bool eal_thread_registered = false;
  if (!eal_thread_registered) {
    if (::rte_lcore_id() == LCORE_ID_ANY) {
      (void)::rte_thread_register();
    }
    eal_thread_registered = true;
  }
  auto cfg_it = cfg_map.find(cb_index);
  if (cfg_it == cfg_map.end()) {
    logger.error("[bbdev_dec] enqueue for cb_index={} without prior configure_operation", cb_index);
    return false;
  }
  const cached_op_config& cfg = cfg_it->second;

  // ACC100 hardware per-op limit: E <= 512 * z_c.
  if (cfg.e > 512U * static_cast<uint32_t>(cfg.z_c)) {
    if (!e_limit_warned) {
      logger.warning(
          "[bbdev_dec] E={} exceeds ACC100 per-op limit 512*z_c={} — op refused, fall back to software",
          cfg.e,
          512U * cfg.z_c);
      e_limit_warned = true;
    }
    return false;
  }
  if (cfg.q_m > 0 && (cfg.e % cfg.q_m) != 0) {
    logger.warning("[bbdev_dec] E={} not multiple of q_m={} — op refused", cfg.e, cfg.q_m);
    return false;
  }

  ::rte_bbdev_dec_op* op = nullptr;
  if (::rte_bbdev_dec_op_alloc_bulk(op_pool->get_pool(), &op, 1) != 0 || op == nullptr) {
    logger.warning("[bbdev_dec] rte_bbdev_dec_op_alloc_bulk failed");
    return false;
  }

  ::rte_mbuf* m_in = ::rte_pktmbuf_alloc(input_pool->get_pool());
  if (m_in == nullptr) {
    logger.warning("[bbdev_dec] input mbuf alloc failed");
    ::rte_bbdev_dec_op_free_bulk(&op, 1);
    return false;
  }
  char* in_buf = ::rte_pktmbuf_append(m_in, data.size());
  if (in_buf == nullptr) {
    logger.warning("[bbdev_dec] input mbuf append failed (size {})", data.size());
    ::rte_pktmbuf_free(m_in);
    ::rte_bbdev_dec_op_free_bulk(&op, 1);
    return false;
  }
  std::memcpy(in_buf, data.data(), data.size());

  ::rte_mbuf* m_hout = ::rte_pktmbuf_alloc(hard_output_pool->get_pool());
  if (m_hout == nullptr) {
    logger.warning("[bbdev_dec] hard-output mbuf alloc failed");
    ::rte_pktmbuf_free(m_in);
    ::rte_bbdev_dec_op_free_bulk(&op, 1);
    return false;
  }

  // ACC100 requires K' (= K - n_filler) byte-aligned.
  const unsigned K        = (cfg.basegraph == 1 ? 22U : 10U) * cfg.z_c;
  unsigned       n_filler = cfg.n_filler;
  const unsigned Kp_raw   = K - n_filler;
  const unsigned Kp_align = (Kp_raw + 7U) & ~7U;
  if (Kp_align > Kp_raw) {
    n_filler = K - Kp_align;
  }

  auto& dec              = op->ldpc_dec;
  dec.input.data         = m_in;
  dec.input.offset       = 0;
  dec.input.length       = data.size();
  dec.hard_output.data   = m_hout;
  dec.hard_output.offset = 0;
  dec.hard_output.length = 0;
  dec.soft_output.data   = nullptr;
  dec.soft_output.length = 0;

  if (!force_local_harq) {
    const uint32_t harq_offset = cfg.absolute_cb_id * static_cast<uint32_t>(HARQ_INCR.value());
    const uint32_t soft_bytes  = (static_cast<uint32_t>(cfg.n_cb) + 7U) / 8U;
    if (harq_buffer_context) {
      auto* ctx = harq_buffer_context->get(cfg.absolute_cb_id, cfg.new_data);
      if (ctx != nullptr) {
        ctx->soft_data_len = soft_bytes;
      }
    }
    dec.harq_combined_input.data    = nullptr;
    dec.harq_combined_input.offset  = harq_offset;
    dec.harq_combined_input.length  = soft_bytes;
    dec.harq_combined_output.data   = nullptr;
    dec.harq_combined_output.offset = harq_offset;
    dec.harq_combined_output.length = soft_bytes;
  }

  dec.op_flags   = cfg.op_flags;
  dec.rv_index   = cfg.rv;
  dec.iter_max   = cfg.max_iter;
  dec.iter_count = 0;
  dec.basegraph  = cfg.basegraph;
  dec.z_c        = cfg.z_c;
  dec.n_cb       = cfg.n_cb;
  dec.q_m        = cfg.q_m;
  dec.n_filler   = static_cast<uint16_t>(n_filler);

  // Per DPDK BBDEV spec: a single-CB TB must be submitted in TB-mode (c=1, cab=1).
  if (cfg.nof_segments <= 1) {
    dec.code_block_mode = RTE_BBDEV_TRANSPORT_BLOCK;
    dec.tb_params.c     = 1;
    dec.tb_params.r     = 0;
    dec.tb_params.cab   = 1;
    dec.tb_params.ea    = cfg.e;
    dec.tb_params.eb    = cfg.e;
  } else {
    dec.code_block_mode = RTE_BBDEV_CODE_BLOCK;
    dec.cb_params.e     = cfg.e;
  }

  op->opaque_data = reinterpret_cast<void*>(static_cast<uintptr_t>(cb_index));

  pending_ops.push_back(op);
  if (pending_ops.size() >= BURST_SIZE) {
    flush_pending();
  }
  return true;
}

void hw_accelerator_pusch_dec_bbdev_impl::flush_pending()
{
  if (pending_ops.empty() || queue_id < 0) {
    return;
  }
  const uint16_t n = ::rte_bbdev_enqueue_ldpc_dec_ops(bbdev_accelerator->get_device_id(),
                                                      static_cast<uint16_t>(queue_id),
                                                      pending_ops.data(),
                                                      static_cast<uint16_t>(pending_ops.size()));
  if (n > 0) {
    in_flight_count += n;
    pending_ops.erase(pending_ops.begin(), pending_ops.begin() + n);
  }
}

void hw_accelerator_pusch_dec_bbdev_impl::poll_dequeue()
{
  if (in_flight_count == 0) {
    return;
  }
  ::rte_bbdev_dec_op* batch[BURST_SIZE];
  const uint16_t      n = ::rte_bbdev_dequeue_ldpc_dec_ops(bbdev_accelerator->get_device_id(),
                                                           static_cast<uint16_t>(queue_id),
                                                           batch,
                                                           BURST_SIZE);
  for (uint16_t i = 0; i < n; ++i) {
    completed_ops.push_back(batch[i]);
  }
  in_flight_count -= n;
}

void hw_accelerator_pusch_dec_bbdev_impl::release_op(::rte_bbdev_dec_op* op)
{
  if (op == nullptr) {
    return;
  }
  if (op->ldpc_dec.input.data != nullptr) {
    ::rte_pktmbuf_free(op->ldpc_dec.input.data);
  }
  if (op->ldpc_dec.hard_output.data != nullptr) {
    ::rte_pktmbuf_free(op->ldpc_dec.hard_output.data);
  }
  ::rte_bbdev_dec_op_free_bulk(&op, 1);
}

bool hw_accelerator_pusch_dec_bbdev_impl::dequeue_operation(span<uint8_t> data,
                                                            span<int8_t>  /*aux_data*/,
                                                            unsigned      /*segment_index*/)
{
  if (completed_ops.empty() && pending_ops.empty() && in_flight_count == 0) {
    logger.warning("[bbdev_dec] dequeue called with no ops outstanding");
    return false;
  }

  constexpr unsigned MAX_ITERS = 1'000'000;
  unsigned           iters     = 0;
  while (completed_ops.empty()) {
    flush_pending();
    poll_dequeue();
    if (++iters > MAX_ITERS) {
      logger.warning("[bbdev_dec] dequeue timeout (pending={}, in_flight={})",
                     pending_ops.size(),
                     in_flight_count);
      return false;
    }
  }

  ::rte_bbdev_dec_op* deq_op = completed_ops.front();
  completed_ops.pop_front();

  const unsigned cb_index = static_cast<unsigned>(reinterpret_cast<uintptr_t>(deq_op->opaque_data));

  cached_op_output res;
  res.iter_count    = deq_op->ldpc_dec.iter_count;
  res.crc_pass      = (deq_op->status == 0);
  out_map[cb_index] = res;

  if (!force_local_harq && harq_buffer_context) {
    auto cfg_it = cfg_map.find(cb_index);
    if (cfg_it != cfg_map.end()) {
      auto* ctx = harq_buffer_context->get(cfg_it->second.absolute_cb_id, false);
      if (ctx != nullptr) {
        ctx->soft_data_len = deq_op->ldpc_dec.harq_combined_output.length;
      }
    }
  }

  if (deq_op->ldpc_dec.hard_output.data != nullptr) {
    ::rte_mbuf*    m_hout  = deq_op->ldpc_dec.hard_output.data;
    const unsigned length  = deq_op->ldpc_dec.hard_output.length;
    const char*    src     = rte_pktmbuf_mtod(m_hout, const char*);
    const size_t   to_copy = std::min(static_cast<size_t>(length), data.size());
    std::memcpy(data.data(), src, to_copy);
  }
  release_op(deq_op);
  return true;
}

void hw_accelerator_pusch_dec_bbdev_impl::read_operation_outputs(hw_pusch_decoder_outputs& out,
                                                                 unsigned                  cb_index,
                                                                 unsigned                  /*absolute_cb_id*/)
{
  auto it = out_map.find(cb_index);
  if (it == out_map.end()) {
    out.CRC_pass            = false;
    out.nof_ldpc_iterations = 0;
    return;
  }
  out.CRC_pass            = it->second.crc_pass;
  out.nof_ldpc_iterations = it->second.iter_count;
}

void hw_accelerator_pusch_dec_bbdev_impl::free_harq_context_entry(unsigned absolute_cb_id)
{
  if (harq_buffer_context) {
    harq_buffer_context->free(absolute_cb_id);
  }
}
