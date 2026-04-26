# SPDX-FileCopyrightText: Copyright (C) 2021-2026 Software Radio Systems Limited
# SPDX-License-Identifier: BSD-3-Clause-Open-MPI
# Portions of this file may implement 3GPP specifications, which may be subject to additional licensing requirements.


add_subdirectory(pusch)

set(HWACC_FACTORIES_DEFINITIONS "")

if (ENABLE_PDSCH_HWACC)
    list(APPEND HWACC_FACTORIES_DEFINITIONS ENABLE_PDSCH_HWACC)
endif ()


set(OCUDU_HAL_PDSCH_SOURCES hw_accelerator_factories.cpp)
if (ENABLE_PDSCH_HWACC)
    list(APPEND OCUDU_HAL_PDSCH_SOURCES hw_accelerator_pdsch_enc_bbdev_impl.cpp)
endif ()

add_library(ocudu_hal_pdsch STATIC ${OCUDU_HAL_PDSCH_SOURCES})

set_source_files_properties(${OCUDU_HAL_PDSCH_SOURCES} PROPERTIES COMPILE_DEFINITIONS "${HWACC_FACTORIES_DEFINITIONS}")
target_compile_options(ocudu_hal_pdsch PRIVATE ${DPDK_CFLAGS})

target_link_libraries(ocudu_hal_pdsch ocudu_hal_bbdev hal_dpdk ${DPDK_LIBRARIES} dl)
