// SPDX-FileCopyrightText: Copyright (C) 2021-2026 Software Radio Systems Limited
// SPDX-License-Identifier: BSD-3-Clause-Open-MPI

#include "ocudu/hal/dpdk/bbdev/bbdev_op_pool_factory.h"
#include <rte_bbdev_op.h>

using namespace ocudu;
using namespace dpdk;

std::unique_ptr<bbdev_op_pool> ocudu::dpdk::create_bbdev_op_pool(const char*             pool_name,
                                                                 ::rte_bbdev_op_type     op_type,
                                                                 uint16_t                nof_elements,
                                                                 int                     socket,
                                                                 ocudulog::basic_logger& logger)
{
  ::rte_mempool* pool = ::rte_bbdev_op_pool_create(pool_name, op_type, nof_elements, 0, socket);
  if (pool == nullptr) {
    logger.error("[bbdev] rte_bbdev_op_pool_create '{}' op_type={} failed", pool_name, (int)op_type);
    return nullptr;
  }
  return std::make_unique<bbdev_op_pool>(pool);
}
