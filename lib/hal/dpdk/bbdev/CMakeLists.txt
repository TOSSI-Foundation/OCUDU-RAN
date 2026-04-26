# SPDX-FileCopyrightText: Copyright (C) 2021-2026 Software Radio Systems Limited
# SPDX-License-Identifier: BSD-3-Clause-Open-MPI


set(SOURCES
  bbdev_acc.cpp
  bbdev_acc_factory.cpp
  bbdev_op_pool_factory.cpp)

add_library(ocudu_hal_bbdev STATIC ${SOURCES})
target_compile_options(ocudu_hal_bbdev PRIVATE ${DPDK_CFLAGS})
target_link_libraries(ocudu_hal_bbdev ocudulog ${DPDK_LIBRARIES} dl)
