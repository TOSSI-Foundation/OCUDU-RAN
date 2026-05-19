# Precompiled xSM shared library. Defines the imported 'xsm' target used by
# ocudu_xsm_context and fapi_xsm_transport. No source is compiled here.

add_library(xsm SHARED IMPORTED GLOBAL)
set_target_properties(xsm PROPERTIES
    IMPORTED_LOCATION             "${CMAKE_CURRENT_SOURCE_DIR}/libxsm.so"
    INTERFACE_INCLUDE_DIRECTORIES "${CMAKE_CURRENT_SOURCE_DIR}")
