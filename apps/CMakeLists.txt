# SPDX-FileCopyrightText: Copyright (C) 2021-2026 Software Radio Systems Limited
# SPDX-License-Identifier: BSD-3-Clause-Open-MPI
# Portions of this file may implement 3GPP specifications, which may be subject to additional licensing requirements.


if (NOT DISABLE_MBEDTLS AND NOT DISABLE_SCTP)
    add_subdirectory(cu)
    add_subdirectory(cu_cp)
    add_subdirectory(cu_up)
    # apps/du_low must precede apps/du so apps/du can link its odu_low_fapi
    # target (FAPI split L1 binary) against odu_low_base defined in apps/du_low.
    add_subdirectory(du_low)
    add_subdirectory(du)
    add_subdirectory(gnb)
endif ()
add_subdirectory(examples)
add_subdirectory(helpers)
add_subdirectory(services)
add_subdirectory(units)
