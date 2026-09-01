# SPDX-FileCopyrightText: Copyright (C) 2021-2026 Software Radio Systems Limited
# SPDX-License-Identifier: BSD-3-Clause-Open-MPI

add_executable(rfsimulator_protocol_test test_rfsim_protocol.cpp)
target_include_directories(rfsimulator_protocol_test PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/../oai_shim
        ${CMAKE_CURRENT_SOURCE_DIR}/..)
target_link_libraries(rfsimulator_protocol_test ocudu_radio_rfsimulator pthread)
add_test(rfsimulator_protocol_test rfsimulator_protocol_test)
set_tests_properties(rfsimulator_protocol_test PROPERTIES TIMEOUT 30)
