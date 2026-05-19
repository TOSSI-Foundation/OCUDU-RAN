# SPDX-FileCopyrightText: Copyright (C) 2021-2026 Software Radio Systems Limited
# SPDX-License-Identifier: BSD-3-Clause-Open-MPI
# Portions of this file may implement 3GPP specifications, which may be subject to additional licensing requirements.


set(SOURCES
        du_appconfig_cli11_schema.cpp
        du_appconfig_validators.cpp
        du_appconfig_translators.cpp
        du_appconfig_yaml_writer.cpp)


add_library(odu_base STATIC ${SOURCES})
target_link_libraries(odu_base PRIVATE
        ocudu_app_helpers
        ocudu_app_services
        ocudu_f1u_du_split_connector
        ocudu_cu_cp
        ocudu_e2
        e2ap_asn1
        ocudu_pcap
        ocudu_support
        ocudu_versioning
        ocudu_f1c_gateway)

if (DPDK_FOUND)
    add_definitions(-DDPDK_FOUND)
    target_link_libraries(odu_base PRIVATE hal_dpdk)
endif (DPDK_FOUND)

target_include_directories(odu_base PRIVATE ${CMAKE_SOURCE_DIR} ${CMAKE_SOURCE_DIR}/external)

# Default DU target with dynamic split.

add_executable(odu du.cpp)
add_backward(odu)
target_include_directories(odu PRIVATE ${CMAKE_SOURCE_DIR})
install(TARGETS odu RUNTIME)
target_link_libraries(odu PRIVATE odu_base ocudu_flexible_o_du_split_dynamic fapi_xsm_transport)
notify_binary_target(odu)


# Add the targets for the rest of the splits. Each split binary also pulls in
# fapi_xsm_transport for the same fapi_stats-symbol reason above.
set(EXTENSIONS du_split_6 du_split_7_2 du_split_8)
set(LIBRARIES ocudu_flexible_o_du_split_6 ocudu_flexible_o_du_split_7_2 ocudu_flexible_o_du_split_8)

foreach (NAME LIB IN ZIP_LISTS EXTENSIONS LIBRARIES)
    # Prepend "o" to the target name to make it consistent with the dynamic one.
    set(TARGET "o${NAME}")
    add_executable(${TARGET} du.cpp)
    add_backward(${TARGET})
    target_include_directories(${TARGET} PRIVATE ${CMAKE_SOURCE_DIR})
    target_link_libraries(${TARGET} PRIVATE odu_base ${LIB} fapi_xsm_transport)
    set_target_properties(${TARGET} PROPERTIES
            RUNTIME_OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/../${NAME}"  # Path to the binary.
            RUNTIME_OUTPUT_NAME "odu"                                        # Binary name.
            EXCLUDE_FROM_ALL 1)                                                # Do not build by default.
    notify_binary_target(${TARGET})
endforeach ()

add_library(fapi_xsm_transport STATIC
        fapi_xsm_transport.cpp
        fapi_stats.cpp
        fapi_stats_serializers.cpp)
target_include_directories(fapi_xsm_transport PUBLIC ${CMAKE_SOURCE_DIR} ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(fapi_xsm_transport PUBLIC
        ocudu_xsm_context
        ocudu_fapi_serialization)
if(ENABLE_XSM_FAPI_SPLIT)
    target_compile_definitions(fapi_xsm_transport PRIVATE ENABLE_XSM_FAPI_SPLIT)
endif()

# --- odu_high: L2 (O-DU High) binary ---
# ENABLE_XSM_FAPI_SPLIT activates xSM init in du.cpp for the FAPI split path.
add_executable(odu_high du.cpp)
add_backward(odu_high)
if(ENABLE_XSM_FAPI_SPLIT)
    target_compile_definitions(odu_high PRIVATE ENABLE_XSM_FAPI_SPLIT)
endif()
target_include_directories(odu_high PRIVATE ${CMAKE_SOURCE_DIR} ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(odu_high PRIVATE
        odu_base
        ocudu_flexible_o_du_split_6
        fapi_xsm_transport)
notify_binary_target(odu_high)


add_executable(odu_low_fapi du_low_main.cpp)
add_backward(odu_low_fapi)
if(ENABLE_XSM_FAPI_SPLIT)
    target_compile_definitions(odu_low_fapi PRIVATE ENABLE_XSM_FAPI_SPLIT)
endif()
target_include_directories(odu_low_fapi PRIVATE ${CMAKE_SOURCE_DIR} ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(odu_low_fapi PRIVATE
        odu_low_base
        ocudu_flexible_o_du_low_split_6
        fapi_xsm_transport)
set_target_properties(odu_low_fapi PROPERTIES
        RUNTIME_OUTPUT_NAME "odu_low")
notify_binary_target(odu_low_fapi)
