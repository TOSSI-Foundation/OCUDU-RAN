# SPDX-FileCopyrightText: Copyright (C) 2021-2026 Software Radio Systems Limited
# SPDX-License-Identifier: BSD-3-Clause-Open-MPI

# They keep their own SPDX headers (LicenseRef-CSSL-1.0); see vendor/README.md.
set(RFSIM_VENDOR ${CMAKE_CURRENT_SOURCE_DIR}/vendor/radio/rfsimulator/simulator.cpp)

add_library(ocudu_radio_rfsimulator STATIC
        ${RFSIM_VENDOR}
        rfsim_shim.cpp
        radio_config_rfsimulator_validator.cpp
        radio_factory_rfsimulator_impl.cpp
        radio_session_rfsimulator_impl.cpp)

target_include_directories(ocudu_radio_rfsimulator PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/oai_shim
        ${CMAKE_CURRENT_SOURCE_DIR}/vendor
        ${CMAKE_CURRENT_SOURCE_DIR})

set_source_files_properties(${RFSIM_VENDOR} PROPERTIES COMPILE_OPTIONS "-w")

target_link_libraries(ocudu_radio_rfsimulator ocudulog ocudu_support pthread)
add_to_exported_libs(ocudu_radio_rfsimulator)

if (BUILD_TESTING)
    add_subdirectory(tests)
endif (BUILD_TESTING)
