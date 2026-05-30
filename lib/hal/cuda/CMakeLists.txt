if (ENABLE_GPU_FRONTHAUL)
    add_library(ocudu_hal_gpu_dpdk_mempool STATIC
        gpu_dpdk_mempool.cpp
        gpu_rx_buffer.cpp
        dpdk_gpu_ethernet_receiver.cpp
        prach_flow_rules.cpp
        vram_prach_buffer.cpp
        vram_srs_buffer.cpp
        prach_inline_bfp_kernel.cu
        srs_inline_bfp_kernel.cu
    )

    set_target_properties(ocudu_hal_gpu_dpdk_mempool PROPERTIES
        CUDA_SEPARABLE_COMPILATION ON
        CUDA_RESOLVE_DEVICE_SYMBOLS ON
        POSITION_INDEPENDENT_CODE ON
    )

    target_include_directories(ocudu_hal_gpu_dpdk_mempool PRIVATE
        ${CMAKE_SOURCE_DIR}/include
        ${CMAKE_SOURCE_DIR}/lib
    )

    target_compile_options(ocudu_hal_gpu_dpdk_mempool PRIVATE ${DPDK_CFLAGS})

    target_link_libraries(ocudu_hal_gpu_dpdk_mempool
        PUBLIC  CUDA::cudart
        PRIVATE ${DPDK_LIBRARIES}
    )

    add_to_exported_libs(ocudu_hal_gpu_dpdk_mempool)

    add_library(ocudu_hal_prach_detector_inline STATIC
        prach_detector_inline_impl.cpp
        prach_detector_inline_kernels.cu
        inline_prach_pipeline.cpp
        srs_estimator_inline_impl.cpp
        srs_inline_estimator_kernels.cu
        inline_srs_pipeline.cpp
    )

    set_target_properties(ocudu_hal_prach_detector_inline PROPERTIES
        POSITION_INDEPENDENT_CODE ON
    )

    target_include_directories(ocudu_hal_prach_detector_inline PRIVATE
        ${CMAKE_SOURCE_DIR}/include
        ${CMAKE_SOURCE_DIR}/lib
    )

    target_link_libraries(ocudu_hal_prach_detector_inline
        PUBLIC  CUDA::cudart CUDA::cufft
        PRIVATE ocudu_hal_gpu_dpdk_mempool ocudu_prach_detector ocudu_ran ocudu_sequence_generators ocudu_srs_estimator ocudu_support
    )

    add_to_exported_libs(ocudu_hal_prach_detector_inline)
endif ()
