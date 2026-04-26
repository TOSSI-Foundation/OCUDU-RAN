# SPDX-FileCopyrightText: Copyright (C) 2021-2026 Software Radio Systems Limited
# SPDX-License-Identifier: BSD-3-Clause-Open-MPI
# Portions of this file may implement 3GPP specifications, which may be subject to additional licensing requirements.


set(HWACC_FACTORIES_DEFINITIONS "")

if (ENABLE_PUSCH_HWACC)
    list(APPEND HWACC_FACTORIES_DEFINITIONS ENABLE_PUSCH_HWACC)
endif ()


set(OCUDU_HAL_PUSCH_SOURCES hw_accelerator_factories.cpp
                            ext_harq_buffer_context_repository_factory.cpp)
if (ENABLE_PUSCH_HWACC)
    list(APPEND OCUDU_HAL_PUSCH_SOURCES hw_accelerator_pusch_dec_bbdev_impl.cpp)
endif ()

add_library(ocudu_hal_pusch STATIC ${OCUDU_HAL_PUSCH_SOURCES})

set_source_files_properties(${OCUDU_HAL_PUSCH_SOURCES} PROPERTIES COMPILE_DEFINITIONS "${HWACC_FACTORIES_DEFINITIONS}")
target_compile_options(ocudu_hal_pusch PRIVATE ${DPDK_CFLAGS})

target_link_libraries(ocudu_hal_pusch ocudu_hal_bbdev hal_dpdk ${DPDK_LIBRARIES} dl)
