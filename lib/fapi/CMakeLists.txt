# SPDX-FileCopyrightText: Copyright (C) 2021-2026 Software Radio Systems Limited
# SPDX-License-Identifier: BSD-3-Clause-Open-MPI


add_subdirectory(decorator_helpers)
add_subdirectory(message_buffering)
add_subdirectory(loggers)
add_subdirectory(serialization)

add_library(ocudu_fapi_decorators STATIC decorator_factory.cpp)
target_link_libraries(ocudu_fapi_decorators PRIVATE ocudu_fapi_message_buffering ocudu_fapi_loggers)
target_include_directories(ocudu_fapi_decorators PRIVATE ${CMAKE_SOURCE_DIR}/lib/fapi)

add_library(ocudu_fapi INTERFACE)
target_link_libraries(ocudu_fapi INTERFACE ocudu_fapi_decorators)
