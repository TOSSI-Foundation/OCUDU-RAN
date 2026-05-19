# SPDX-FileCopyrightText: Copyright (C) 2021-2026 Software Radio Systems Limited
# SPDX-License-Identifier: BSD-3-Clause-Open-MPI
# Portions of this file may implement 3GPP specifications, which may be subject to additional licensing requirements.


set(SOURCES
        slot_point_extender_adaptor.cpp
        split6_o_du_application_unit_impl.cpp
        split6_o_du_factory.cpp
        split6_o_du_impl.cpp
        split6_o_du_unit_cli11_schema.cpp
        split6_o_du_unit_config_validator.cpp)

set(PLUGIN_SOURCES split6_plugin_dummy.cpp)
if(ENABLE_XSM_FAPI_SPLIT)
    list(APPEND PLUGIN_SOURCES split6_plugin_xsm.cpp)
endif()

add_library(ocudu_split6_plugin STATIC ${PLUGIN_SOURCES})
target_include_directories(ocudu_split6_plugin PRIVATE ${CMAKE_SOURCE_DIR})
if(ENABLE_XSM_FAPI_SPLIT)
    target_compile_definitions(ocudu_split6_plugin PUBLIC ENABLE_XSM_FAPI_SPLIT)
    target_link_libraries(ocudu_split6_plugin PUBLIC fapi_xsm_transport)
endif()

add_library(ocudu_flexible_o_du_split_6 STATIC ${SOURCES})
target_include_directories(ocudu_flexible_o_du_split_6 PRIVATE ${CMAKE_SOURCE_DIR})
target_link_libraries(ocudu_flexible_o_du_split_6
                                                    ocudu_o_du
                                                    ocudu_o_du_high_unit_helpers
                                                    ocudu_split6_plugin
                                                    ocudu_metrics_json_generators)
