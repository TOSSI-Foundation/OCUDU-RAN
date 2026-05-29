# SPDX-FileCopyrightText: Copyright (C) 2021-2026 Software Radio Systems Limited
# SPDX-License-Identifier: BSD-3-Clause-Open-MPI


set(SOURCES
        dpdk_ethernet_port_context.cpp
        dpdk_ethernet_transmitter.cpp
        dpdk_ethernet_factories.cpp
        dpdk_ethernet_receiver.cpp)

add_library(ocudu_ofh_dpdk_ethernet STATIC ${SOURCES})
target_compile_options(ocudu_ofh_dpdk_ethernet PRIVATE ${DPDK_CFLAGS})
target_include_directories(ocudu_ofh_dpdk_ethernet PRIVATE ${CMAKE_SOURCE_DIR}/include)
target_link_libraries(ocudu_ofh_dpdk_ethernet ocudulog ocudu_instrumentation ${DPDK_LIBRARIES})

# GPU fronthaul: port_context grows an optional 2-queue + GPU mempool + listener thread when
# ENABLE_GPU_FRONTHAUL=ON. The new code path is guarded by the #ifdef; the link dep is added
# only when the feature is on so OFF builds are unchanged.
if (ENABLE_GPU_FRONTHAUL)
    target_link_libraries(ocudu_ofh_dpdk_ethernet ocudu_hal_gpu_dpdk_mempool CUDA::cudart)
endif ()
