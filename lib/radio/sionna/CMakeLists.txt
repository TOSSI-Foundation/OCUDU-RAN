

set(SOURCES_SIONNA
        radio_factory_sionna_impl.cpp
        radio_session_sionna_impl.cpp
        radio_sionna_baseband_gateway.cpp)
add_library(ocudu_radio_sionna STATIC ${SOURCES_SIONNA})

target_link_libraries(ocudu_radio_sionna ocudu_radio_zmq ocudu_channel_sionna fmt ocudulog ocuduvec ocudu_support)

add_to_exported_libs(ocudu_radio_sionna)
