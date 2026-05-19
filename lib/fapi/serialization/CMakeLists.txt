set(SOURCES
        fapi_serializer_p5.cpp
        fapi_serializer_p7_dl.cpp
        fapi_serializer_p7_ul.cpp
        fapi_serializer_p7_ind.cpp)

add_library(ocudu_fapi_serialization STATIC ${SOURCES})
target_include_directories(ocudu_fapi_serialization PRIVATE ${CMAKE_SOURCE_DIR})
target_link_libraries(ocudu_fapi_serialization PUBLIC ocudu_ran ocudu_support)
