// SPDX-FileCopyrightText: Copyright (C) 2021-2026 Software Radio Systems Limited
// SPDX-License-Identifier: BSD-3-Clause-Open-MPI

#include "ocudu/hal/dpdk/bbdev/bbdev_acc.h"
#include "ocudu/support/ocudu_assert.h"
#include <rte_bbdev.h>

using namespace ocudu;
using namespace dpdk;

namespace {

static int configure_and_start_queue(unsigned                      dev_id,
                                     unsigned                      queue_id,
                                     int                           socket_id,
                                     ::rte_bbdev_op_type           op_type,
                                     const ::rte_bbdev_queue_conf& default_qconf,
                                     ocudulog::basic_logger&       logger)
{
  ::rte_bbdev_queue_conf qconf = default_qconf;
  qconf.socket                 = socket_id;
  qconf.op_type                = op_type;

  int ret = ::rte_bbdev_queue_configure(dev_id, queue_id, &qconf);
  if (ret < 0) {
    logger.warning(
        "[bbdev] rte_bbdev_queue_configure dev={} q={} op_type={} failed: {}", dev_id, queue_id, (int)op_type, ret);
    return -1;
  }
  ret = ::rte_bbdev_queue_start(dev_id, queue_id);
  if (ret < 0) {
    logger.warning("[bbdev] rte_bbdev_queue_start dev={} q={} failed: {}", dev_id, queue_id, ret);
    return -1;
  }
  return static_cast<int>(queue_id);
}

} // namespace

bbdev_acc::bbdev_acc(const bbdev_acc_configuration& cfg,
                     const ::rte_bbdev_info&        info_,
                     ocudulog::basic_logger&        logger_) :
  id(cfg.id),
  info(info_),
  nof_ldpc_enc_lcores(cfg.nof_ldpc_enc_lcores),
  nof_ldpc_dec_lcores(cfg.nof_ldpc_dec_lcores),
  nof_fft_lcores(cfg.nof_fft_lcores),
  available_ldpc_enc_queue(MAX_NOF_BBDEV_QUEUES),
  available_ldpc_dec_queue(MAX_NOF_BBDEV_QUEUES),
  available_fft_queue(MAX_NOF_BBDEV_QUEUES),
  msg_mbuf_size(cfg.msg_mbuf_size),
  rm_mbuf_size(cfg.rm_mbuf_size),
  nof_mbuf(cfg.nof_mbuf),
  logger(logger_),
  nof_ldpc_enc_instances(0),
  nof_ldpc_dec_instances(0)
{
  const unsigned nof_queues = nof_ldpc_enc_lcores + nof_ldpc_dec_lcores + nof_fft_lcores;
  ocudu_assert(nof_queues > 0, "bbdev_acc created with zero queues configured");
  ocudu_assert(nof_queues <= MAX_NOF_BBDEV_QUEUES,
               "Requested {} queues exceeds MAX_NOF_BBDEV_QUEUES={}",
               nof_queues,
               MAX_NOF_BBDEV_QUEUES);

  int ret = ::rte_bbdev_setup_queues(id, nof_queues, info.socket_id);
  if (ret < 0) {
    logger.error("[bbdev] rte_bbdev_setup_queues(dev={}, nof_queues={}) failed: {}", id, nof_queues, ret);
    return;
  }

  const ::rte_bbdev_queue_conf default_qconf = info.drv.default_queue_conf;

  unsigned next_queue_id = 0;

  for (unsigned i = 0; i < nof_ldpc_enc_lcores; ++i) {
    int q = configure_and_start_queue(
        id, next_queue_id, info.socket_id, RTE_BBDEV_OP_LDPC_ENC, default_qconf, logger);
    if (q < 0) {
      break;
    }
    (void)available_ldpc_enc_queue.try_push(static_cast<unsigned>(q));
    ++next_queue_id;
  }
  for (unsigned i = 0; i < nof_ldpc_dec_lcores; ++i) {
    int q = configure_and_start_queue(
        id, next_queue_id, info.socket_id, RTE_BBDEV_OP_LDPC_DEC, default_qconf, logger);
    if (q < 0) {
      break;
    }
    (void)available_ldpc_dec_queue.try_push(static_cast<unsigned>(q));
    ++next_queue_id;
  }
  for (unsigned i = 0; i < nof_fft_lcores; ++i) {
    int q = configure_and_start_queue(id, next_queue_id, info.socket_id, RTE_BBDEV_OP_FFT, default_qconf, logger);
    if (q < 0) {
      break;
    }
    (void)available_fft_queue.try_push(static_cast<unsigned>(q));
    ++next_queue_id;
  }

  ret = ::rte_bbdev_start(id);
  if (ret < 0) {
    logger.error("[bbdev] rte_bbdev_start(dev={}) failed: {}", id, ret);
    return;
  }

  logger.info("[bbdev] dev={} started: ldpc_enc_q={} ldpc_dec_q={} fft_q={} socket={}",
              id,
              nof_ldpc_enc_lcores,
              nof_ldpc_dec_lcores,
              nof_fft_lcores,
              info.socket_id);
}

bbdev_acc::~bbdev_acc()
{
  ::rte_bbdev_stop(id);
  ::rte_bbdev_close(id);
  logger.info("[bbdev] dev={} closed", id);
}

int bbdev_acc::reserve_queue(::rte_bbdev_op_type op_type)
{
  unsigned queue_id = 0;
  bool     popped   = false;
  switch (op_type) {
    case RTE_BBDEV_OP_LDPC_ENC:
      popped = available_ldpc_enc_queue.try_pop(queue_id);
      break;
    case RTE_BBDEV_OP_LDPC_DEC:
      popped = available_ldpc_dec_queue.try_pop(queue_id);
      break;
    case RTE_BBDEV_OP_FFT:
      popped = available_fft_queue.try_pop(queue_id);
      break;
    default:
      logger.warning("[bbdev] reserve_queue called with unsupported op_type={}", (int)op_type);
      return -1;
  }
  if (!popped) {
    logger.warning("[bbdev] no free queue available for op_type={}", (int)op_type);
    return -1;
  }
  return static_cast<int>(queue_id);
}

void bbdev_acc::free_queue(::rte_bbdev_op_type op_type, unsigned queue_id)
{
  switch (op_type) {
    case RTE_BBDEV_OP_LDPC_ENC:
      (void)available_ldpc_enc_queue.try_push(queue_id);
      break;
    case RTE_BBDEV_OP_LDPC_DEC:
      (void)available_ldpc_dec_queue.try_push(queue_id);
      break;
    case RTE_BBDEV_OP_FFT:
      (void)available_fft_queue.try_push(queue_id);
      break;
    default:
      logger.warning("[bbdev] free_queue called with unsupported op_type={}", (int)op_type);
      break;
  }
}
