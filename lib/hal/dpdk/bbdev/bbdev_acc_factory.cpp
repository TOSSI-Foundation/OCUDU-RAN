// SPDX-FileCopyrightText: Copyright (C) 2021-2026 Software Radio Systems Limited
// SPDX-License-Identifier: BSD-3-Clause-Open-MPI

#include "ocudu/hal/dpdk/bbdev/bbdev_acc_factory.h"
#include <rte_bbdev.h>

using namespace ocudu;
using namespace dpdk;

std::shared_ptr<bbdev_acc> ocudu::dpdk::create_bbdev_acc(const bbdev_acc_configuration& cfg,
                                                         ocudulog::basic_logger&        logger)
{
  const unsigned nof_devs = ::rte_bbdev_count();
  if (nof_devs == 0) {
    logger.error("[bbdev] no bbdev devices available. EAL initialized? PCIe device allowlisted?");
    return nullptr;
  }
  if (cfg.id >= nof_devs) {
    logger.error("[bbdev] device id {} out of range (nof_devs={})", cfg.id, nof_devs);
    return nullptr;
  }

  ::rte_bbdev_info info;
  if (::rte_bbdev_info_get(cfg.id, &info) < 0) {
    logger.error("[bbdev] rte_bbdev_info_get(dev={}) failed", cfg.id);
    return nullptr;
  }

  logger.info("[bbdev] dev={} driver={} pci={} max_queues={} hw_queues={}",
              cfg.id,
              info.drv.driver_name,
              info.dev_name,
              info.drv.max_num_queues,
              info.num_queues);

  const unsigned requested = cfg.nof_ldpc_enc_lcores + cfg.nof_ldpc_dec_lcores + cfg.nof_fft_lcores;
  if (requested > info.drv.max_num_queues) {
    logger.error("[bbdev] requested {} queues but device supports only {}", requested, info.drv.max_num_queues);
    return nullptr;
  }

  return std::make_shared<bbdev_acc>(cfg, info, logger);
}
