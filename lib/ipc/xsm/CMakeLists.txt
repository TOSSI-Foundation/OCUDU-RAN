add_library(ocudu_xsm_context STATIC xsm_context.cpp)
target_include_directories(ocudu_xsm_context PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}
        ${CMAKE_SOURCE_DIR})

target_link_libraries(ocudu_xsm_context PUBLIC
        xsm
        pthread
        ${DPDK_LIBRARIES})
