add_library(ocudu_channel_sionna STATIC
        cir_artifact.cpp
        cir_zmq_receiver.cpp
        sionna_channel_engine.cpp)

target_link_libraries(ocudu_channel_sionna ${ZEROMQ_LIBRARIES} fmt ocudulog ocuduvec ocudu_support)

add_to_exported_libs(ocudu_channel_sionna)
