<!--
SPDX-FileCopyrightText: Copyright (C) 2026 Coran Labs
SPDX-License-Identifier: BSD-3-Clause-Open-MPI
-->

# OCUDU



**OCUDU India : An integrated open telecom ecosystem.**

At OCUDU India, our goal is to go beyond a standalone RAN stack and build a fully integrated open telecom ecosystem.

We actively work toward bringing together key open-source and emerging technologies across the stack:

- **Core networks** — Magma Core · Aether SD-Core · Free5GC
- **RAN & orchestration** — O-RAN Software Community · SMO · ONAP
- **Intelligence & automation** — AI-RAN · Digital Twin Networks
- **Security** — Post-Quantum Cryptography (PQC)
- **Infrastructure & interfaces** — Open M-Plane · RISC-V

This repository is a **fork of [OCUDU](https://gitlab.com/ocudu/ocudu)** that serves as the integration point for the work above. Upstream RAN functionality is preserved end-to-end - the same L1/2/3 stack, the same 3GPP and O-RAN Alliance compliance with each integration staged on a dedicated branch as it matures.

## Upstream

- **Project:** OCUDU (Linux Foundation, governed by the OCUDU Technical Steering Committee)
- **Source:** <https://gitlab.com/ocudu/ocudu>
- **Documentation:** <https://docs.ocudu.org> · <https://docs.ocuduindia.org>


## Branches

| Branch | Status | Contents |
|---|---|---|
| **[`fapi_split`](../../tree/fapi_split)** | Active | **FAPI L1/L2 split with xFAPI translator-bridge**. Adds a serialized P5/P7 FAPI transport over **xSM (DPDK-backed shared memory)**, enabling `odu_high` (L2/MAC) and `odu_low` (L1/PHY) to run as separate processes while remaining wire-compatible with upstream OCUDU FAPI. Includes an **xFAPI translator-bridge** that decouples L1 and L2 endpoints, enabling deployments across different DPDK domains or even separate hosts. Supports both **single-host bridge** and **two-host xFAPI split** topologies, with configuration-driven activation via YAML (`fapi_split_l1` / `fapi_split_l2`) while preserving the same monolithic deployment flow. See the branch's [`README.md`](../../blob/fapi_split/README.md) and [`docs/fapi_split_topologies.md`](../../blob/fapi_split/docs/fapi_split_topologies.md) for deployment details and startup flow. |
| **[`hwacc_acc100`](../../tree/hwacc_acc100)** | Active | **Intel ACC100 LDPC hardware offload** via DPDK BBDEV. Adds hardware-accelerated PDSCH encode and PUSCH decode (the two heaviest channel-coding stages), a unified PHY metrics layer that lets operators A/B compare software vs. HW paths in one log format, and a full operator deployment guide. See the branch's [`README.md`](../../blob/hwacc_acc100/README.md) and [`docs/DEPLOYMENT.md`](../../blob/hwacc_acc100/docs/DEPLOYMENT.md) for details and measured results. |
| **[`hwacc_gpu`](../../tree/hwacc_gpu)** | Active | **NVIDIA GPU PRACH detection offload** via cuFFTDx fused kernel + CUDA graph capture. Replaces the AVX-512 FFTW PRACH inner loop (correlation, IDFT, power accumulation, peak detection) with a device-resident pipeline launched as a single `cudaGraphLaunch`. Selectable at runtime via the `OCUDU_PRACH_DFT_BACKEND=gpu_full` environment variable; reuses the same unified PHY metrics layer for direct A/B comparison. See the branch's [`README.md`](../../blob/hwacc_gpu/README.md) and [`docs/DEPLOYMENT.md`](../../blob/hwacc_gpu/docs/DEPLOYMENT.md) for details and measured results. |

Additional branches for further accelerator work, deployment tooling, and downstream integrations will be added here as they land.


## License

BSD 3-Clause Open MPI variant — see the [LICENSE](./LICENSE) file. Portions of this software may implement 3GPP specifications, which may be subject to additional licensing requirements.
