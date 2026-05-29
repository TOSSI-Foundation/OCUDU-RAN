# SPDX-FileCopyrightText: Copyright (C) 2021-2026 Software Radio Systems Limited
# SPDX-License-Identifier: BSD-3-Clause-Open-MPI


add_subdirectory(compression)
add_subdirectory(ecpri)
add_subdirectory(ethernet)
add_subdirectory(receiver)
add_subdirectory(serdes)
add_subdirectory(timing)
add_subdirectory(transmitter)

add_library(ocudu_ofh ofh_factories.cpp ofh_metrics_collector_impl.cpp ofh_sector_impl.cpp ofh_sector_controller.cpp)
target_link_libraries(ocudu_ofh ocudu_ofh_receiver ocudu_ofh_timing ocudu_ofh_transmitter ocudu_ofh_ethernet)

if (DPDK_FOUND)
    add_definitions(-DDPDK_FOUND)
endif (DPDK_FOUND)

# Phase 6C.2 — the OFH factory constructs the inline GPU PRACH pipeline when the YAML flag
# `prach_rx_to_gpu: true` is set on a cell. Pull in the inline detector + aggregator lib so
# `hal::cuda::inline_prach_pipeline::create()` resolves at link time. Linking is conditional
# on the same flag the existing GPU mempool code paths use, so a non-GPU build is unaffected.
if (ENABLE_GPU_FRONTHAUL)
    target_link_libraries(ocudu_ofh ocudu_hal_prach_detector_inline)
endif ()
