set_directory_properties(PROPERTIES LABELS "gpu_fronthaul")

add_executable(srs_inline_benchmark srs_inline_benchmark.cpp)

target_include_directories(srs_inline_benchmark PRIVATE ${CMAKE_SOURCE_DIR}/include)

target_link_libraries(srs_inline_benchmark
    PRIVATE
        ocudu_hal_prach_detector_inline
        ocudu_hal_gpu_dpdk_mempool
        ocudu_srs_estimator
        ocudu_sequence_generators
        ocudu_phy_support
        ocudu_dft
        ocudu_ran
        ocudulog
        ocudu_support
        CUDA::cudart
)

install(TARGETS srs_inline_benchmark RUNTIME)
