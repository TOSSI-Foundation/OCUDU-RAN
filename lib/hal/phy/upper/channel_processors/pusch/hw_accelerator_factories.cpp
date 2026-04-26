// SPDX-FileCopyrightText: Copyright (C) 2021-2026 Software Radio Systems Limited
// SPDX-License-Identifier: BSD-3-Clause-Open-MPI
// Portions of this file may implement 3GPP specifications, which may be subject to additional licensing requirements.

#include "ocudu/hal/phy/upper/channel_processors/pusch/hw_accelerator_factories.h"
#ifdef ENABLE_PUSCH_HWACC
#include "hw_accelerator_pusch_dec_bbdev_impl.h"
#include "ocudu/hal/dpdk/bbdev/bbdev_op_pool_factory.h"
#include "ocudu/hal/dpdk/mbuf_pool_factory.h"
#include "ocudu/support/ocudu_assert.h"
#include <algorithm>
#include <atomic>
#include <rte_bbdev_op.h>
#endif

using namespace ocudu;
using namespace hal;

#ifdef ENABLE_PUSCH_HWACC

namespace {

class bbdev_hwacc_pusch_dec_factory : public hw_accelerator_pusch_dec_factory
{
public:
  bbdev_hwacc_pusch_dec_factory(std::vector<std::shared_ptr<dpdk::bbdev_acc>>       bbdev_accelerators_,
                                std::shared_ptr<dpdk::mbuf_pool>                    input_pool_,
                                std::shared_ptr<dpdk::mbuf_pool>                    hard_output_pool_,
                                std::shared_ptr<dpdk::bbdev_op_pool>                op_pool_,
                                std::shared_ptr<ext_harq_buffer_context_repository> harq_buffer_context_,
                                bool                                                force_local_harq_,
                                bool                                                dedicated_queue_) :
    bbdev_accelerators(std::move(bbdev_accelerators_)),
    input_pool(std::move(input_pool_)),
    hard_output_pool(std::move(hard_output_pool_)),
    op_pool(std::move(op_pool_)),
    harq_buffer_context(std::move(harq_buffer_context_)),
    force_local_harq(force_local_harq_),
    dedicated_queue(dedicated_queue_)
  {
  }

  std::unique_ptr<hw_accelerator_pusch_dec> create() override
  {
    const unsigned idx       = next_device.fetch_add(1, std::memory_order_relaxed) % bbdev_accelerators.size();
    auto&          bbdev_acc = bbdev_accelerators[idx];

    const unsigned decoder_id = bbdev_acc->reserve_decoder();
    return std::make_unique<hw_accelerator_pusch_dec_bbdev_impl>(
        bbdev_acc, input_pool, hard_output_pool, op_pool,
        harq_buffer_context, force_local_harq, decoder_id, dedicated_queue);
  }

private:
  std::vector<std::shared_ptr<dpdk::bbdev_acc>>       bbdev_accelerators;
  std::shared_ptr<dpdk::mbuf_pool>                    input_pool;
  std::shared_ptr<dpdk::mbuf_pool>                    hard_output_pool;
  std::shared_ptr<dpdk::bbdev_op_pool>                op_pool;
  std::shared_ptr<ext_harq_buffer_context_repository> harq_buffer_context;
  bool                                                force_local_harq;
  bool                                                dedicated_queue;
  std::atomic<unsigned>                               next_device{0};
};

} // namespace

std::shared_ptr<hw_accelerator_pusch_dec_factory>
ocudu::hal::create_bbdev_pusch_dec_acc_factory(const bbdev_hwacc_pusch_dec_factory_configuration& cfg)
{
  if (!cfg.bbdev_accelerator) {
    return nullptr;
  }

  std::vector<std::shared_ptr<dpdk::bbdev_acc>> bbdev_accelerators;
  bbdev_accelerators.reserve(1 + cfg.additional_bbdev_accelerators.size());
  bbdev_accelerators.push_back(cfg.bbdev_accelerator);
  for (const auto& acc : cfg.additional_bbdev_accelerators) {
    if (acc) {
      bbdev_accelerators.push_back(acc);
    }
  }

  auto&          logger        = bbdev_accelerators.front()->get_logger();
  const int      socket_id     = bbdev_accelerators.front()->get_socket_id();
  const unsigned base_n_mbuf   = bbdev_accelerators.front()->get_nof_mbuf();
  unsigned       total_queues  = 0;
  for (const auto& acc : bbdev_accelerators) {
    total_queues += std::max(1U, acc->get_nof_ldpc_dec_cores());
  }

  constexpr unsigned MEMPOOL_CACHE = 32;
  const unsigned     n_mbuf        = std::max<unsigned>(base_n_mbuf, total_queues * base_n_mbuf);

  if (bbdev_accelerators.size() > 1) {
    logger.info("[bbdev_dec] factory: multi-VF mode with {} bbdev devices, {} total dec queues, pool size {}",
                bbdev_accelerators.size(),
                total_queues,
                n_mbuf);
  }

  dpdk::mempool_config in_cfg;
  in_cfg.n_mbuf             = n_mbuf;
  in_cfg.mempool_cache_size = MEMPOOL_CACHE;
  in_cfg.mbuf_data_size     = bbdev_accelerators.front()->get_rm_mbuf_size().value();
  std::shared_ptr<dpdk::mbuf_pool> input_pool =
      dpdk::create_mbuf_pool("pusch_dec_llr", socket_id, in_cfg, logger);
  if (!input_pool) {
    logger.error("[bbdev_dec] factory: failed to create shared input LLR mbuf pool");
    return nullptr;
  }

  dpdk::mempool_config out_cfg;
  out_cfg.n_mbuf             = n_mbuf;
  out_cfg.mempool_cache_size = MEMPOOL_CACHE;
  out_cfg.mbuf_data_size     = bbdev_accelerators.front()->get_msg_mbuf_size().value();
  std::shared_ptr<dpdk::mbuf_pool> hard_output_pool =
      dpdk::create_mbuf_pool("pusch_dec_hout", socket_id, out_cfg, logger);
  if (!hard_output_pool) {
    logger.error("[bbdev_dec] factory: failed to create shared hard-output mbuf pool");
    return nullptr;
  }

  std::shared_ptr<dpdk::bbdev_op_pool> op_pool =
      dpdk::create_bbdev_op_pool("pusch_dec_op", RTE_BBDEV_OP_LDPC_DEC, n_mbuf, socket_id, logger);
  if (!op_pool) {
    logger.error("[bbdev_dec] factory: failed to create shared bbdev op pool");
    return nullptr;
  }

  return std::make_shared<bbdev_hwacc_pusch_dec_factory>(
      std::move(bbdev_accelerators),
      std::move(input_pool),
      std::move(hard_output_pool),
      std::move(op_pool),
      cfg.harq_buffer_context,
      cfg.force_local_harq,
      cfg.dedicated_queue);
}

#else // ENABLE_PUSCH_HWACC

std::shared_ptr<hw_accelerator_pusch_dec_factory>
ocudu::hal::create_bbdev_pusch_dec_acc_factory(const bbdev_hwacc_pusch_dec_factory_configuration& /*cfg*/)
{
  return nullptr;
}

#endif // ENABLE_PUSCH_HWACC
