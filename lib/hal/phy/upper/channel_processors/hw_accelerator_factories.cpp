// SPDX-FileCopyrightText: Copyright (C) 2021-2026 Software Radio Systems Limited
// SPDX-License-Identifier: BSD-3-Clause-Open-MPI
// Portions of this file may implement 3GPP specifications, which may be subject to additional licensing requirements.

#include "ocudu/hal/phy/upper/channel_processors/hw_accelerator_factories.h"
#ifdef ENABLE_PDSCH_HWACC
#include "hw_accelerator_pdsch_enc_bbdev_impl.h"
#include "ocudu/hal/dpdk/bbdev/bbdev_op_pool_factory.h"
#include "ocudu/hal/dpdk/mbuf_pool_factory.h"
#include "ocudu/support/ocudu_assert.h"
#include <algorithm>
#include <atomic>
#include <rte_bbdev_op.h>
#endif

using namespace ocudu;
using namespace hal;

#ifdef ENABLE_PDSCH_HWACC

namespace {

class bbdev_hwacc_pdsch_enc_factory : public hw_accelerator_pdsch_enc_factory
{
public:
  bbdev_hwacc_pdsch_enc_factory(std::vector<std::shared_ptr<dpdk::bbdev_acc>> bbdev_accelerators_,
                                std::shared_ptr<dpdk::mbuf_pool>              input_pool_,
                                std::shared_ptr<dpdk::mbuf_pool>              output_pool_,
                                std::shared_ptr<dpdk::bbdev_op_pool>          op_pool_,
                                bool                                          dedicated_queue_) :
    bbdev_accelerators(std::move(bbdev_accelerators_)),
    input_pool(std::move(input_pool_)),
    output_pool(std::move(output_pool_)),
    op_pool(std::move(op_pool_)),
    dedicated_queue(dedicated_queue_)
  {
  }

  std::unique_ptr<hw_accelerator_pdsch_enc> create() override
  {
    const unsigned idx       = next_device.fetch_add(1, std::memory_order_relaxed) % bbdev_accelerators.size();
    auto&          bbdev_acc = bbdev_accelerators[idx];

    const unsigned encoder_id = bbdev_acc->reserve_encoder();
    return std::make_unique<hw_accelerator_pdsch_enc_bbdev_impl>(
        bbdev_acc, input_pool, output_pool, op_pool, encoder_id, dedicated_queue);
  }

private:
  std::vector<std::shared_ptr<dpdk::bbdev_acc>> bbdev_accelerators;
  std::shared_ptr<dpdk::mbuf_pool>              input_pool;
  std::shared_ptr<dpdk::mbuf_pool>              output_pool;
  std::shared_ptr<dpdk::bbdev_op_pool>          op_pool;
  bool                                          dedicated_queue;
  std::atomic<unsigned>                         next_device{0};
};

} // namespace

std::shared_ptr<hw_accelerator_pdsch_enc_factory>
ocudu::hal::create_bbdev_pdsch_enc_acc_factory(const bbdev_hwacc_pdsch_enc_factory_configuration& cfg)
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
    total_queues += std::max(1U, acc->get_nof_ldpc_enc_cores());
  }

  constexpr unsigned MEMPOOL_CACHE = 32;

  const unsigned n_mbuf = std::max<unsigned>(base_n_mbuf, total_queues * base_n_mbuf);

  if (bbdev_accelerators.size() > 1) {
    logger.info("[bbdev_enc] factory: multi-VF mode with {} bbdev devices, {} total enc queues, pool size {}",
                bbdev_accelerators.size(),
                total_queues,
                n_mbuf);
  }

  dpdk::mempool_config in_cfg;
  in_cfg.n_mbuf             = n_mbuf;
  in_cfg.mempool_cache_size = MEMPOOL_CACHE;
  in_cfg.mbuf_data_size     = bbdev_accelerators.front()->get_msg_mbuf_size().value();
  std::shared_ptr<dpdk::mbuf_pool> input_pool =
      dpdk::create_mbuf_pool("pdsch_enc_in", socket_id, in_cfg, logger);
  if (!input_pool) {
    logger.error("[bbdev_enc] factory: failed to create shared input mbuf pool");
    return nullptr;
  }

  dpdk::mempool_config out_cfg;
  out_cfg.n_mbuf             = n_mbuf;
  out_cfg.mempool_cache_size = MEMPOOL_CACHE;
  out_cfg.mbuf_data_size     = bbdev_accelerators.front()->get_rm_mbuf_size().value();
  std::shared_ptr<dpdk::mbuf_pool> output_pool =
      dpdk::create_mbuf_pool("pdsch_enc_out", socket_id, out_cfg, logger);
  if (!output_pool) {
    logger.error("[bbdev_enc] factory: failed to create shared output mbuf pool");
    return nullptr;
  }

  std::shared_ptr<dpdk::bbdev_op_pool> op_pool =
      dpdk::create_bbdev_op_pool("pdsch_enc_op", RTE_BBDEV_OP_LDPC_ENC, n_mbuf, socket_id, logger);
  if (!op_pool) {
    logger.error("[bbdev_enc] factory: failed to create shared bbdev op pool");
    return nullptr;
  }

  return std::make_shared<bbdev_hwacc_pdsch_enc_factory>(
      std::move(bbdev_accelerators), std::move(input_pool), std::move(output_pool), std::move(op_pool), cfg.dedicated_queue);
}

#else // ENABLE_PDSCH_HWACC

std::shared_ptr<hw_accelerator_pdsch_enc_factory>
ocudu::hal::create_bbdev_pdsch_enc_acc_factory(const bbdev_hwacc_pdsch_enc_factory_configuration& /*cfg*/)
{
  return nullptr;
}

#endif // ENABLE_PDSCH_HWACC
