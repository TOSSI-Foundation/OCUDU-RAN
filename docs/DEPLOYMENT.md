<!--
SPDX-FileCopyrightText: Copyright (C) 2021-2026 Software Radio Systems Limited
SPDX-License-Identifier: BSD-3-Clause-Open-MPI
-->

# OCUDU - GPU PRACH Deployment Guide

This guide covers the procedure to install, build, configure, and verify the
GPU-accelerated PRACH detection path. It assumes familiarity with the
upstream OCUDU build flow.

---

## 1. Prerequisites

### 1.1 Hardware

- **NVIDIA Ampere or later GPU (SM86+)** for the cuFFTDx fast path.
  Tested: RTX A4000 (SM86, fused path) and RTX 8000 (SM75, falls back to
  cuFFT three-kernel path automatically).
- ≥ **512 MB free GPU device memory** (worst-case allocation ~300 MB for
  64 ports × 12 symbols × 4096-bin DFT).
- GPU on the **same NUMA node** as upper-PHY worker cores.

### 1.2 Software

| Component | Minimum | Notes |
|---|---|---|
| NVIDIA driver | 525+ | |
| CUDA Toolkit | 12.0 | `nvcc`, `cuda_runtime.h`, `cufft.h` |
| NVIDIA MathDx | 26.03 | `cufftdx.hpp` at `/opt/nvidia-mathdx-26.03.0-cuda13/` |
| Linux kernel | 5.15+ | Ubuntu 22.04 LTS or equivalent |
| CMake | 3.18+ | For `CUDA_SEPARABLE_COMPILATION` |
| Build flag | `ENABLE_DFT_GPU=ON` | |

---

## 2. Driver and CUDA install

```bash
# Verify GPU + driver:
nvidia-smi    # expect driver ≥ 525, CUDA ≥ 12.0

# Verify CUDA Toolkit:
nvcc --version

# Install CUDA Toolkit if missing (Ubuntu 22.04):
wget https://developer.download.nvidia.com/compute/cuda/repos/ubuntu2204/x86_64/cuda-keyring_1.1-1_all.deb
sudo dpkg -i cuda-keyring_1.1-1_all.deb
sudo apt-get update
sudo apt-get install -y cuda-toolkit-12-2
```

---

## 3. NVIDIA MathDx (cuFFTDx) install

cuFFTDx is header-only. The build system looks for it under
`/opt/nvidia-mathdx-26.03.0-cuda13/`.

```bash
# Download from https://developer.nvidia.com/mathdx-downloads
# (login required; select "MathDx 26.03 for CUDA 13")

sudo tar -xf nvidia-mathdx-26.03.0-cuda13.tar.gz -C /opt/

# Verify CMake config is reachable:
ls /opt/nvidia-mathdx-26.03.0-cuda13/nvidia/mathdx/26.03/lib/cmake/
# Expect: mathdxConfig.cmake  cufftdxConfig.cmake
```

---

## 4. GPU runtime tuning (recommended for production)

```bash
# Persistence mode (avoids cold init on first CUDA call):
sudo nvidia-smi -pm 1

# Lock clocks for deterministic latency (RTX A4000 example):
sudo nvidia-smi -lgc 1440
sudo nvidia-smi -lmc 1215

# Disable boost throttling:
sudo nvidia-smi --auto-boost-default=0
```

Persistence mode and clock-locking eliminate frequency-scaling jitter that
otherwise causes occasional tail-latency outliers.

---

## 5. Build

### 5.1 SPLIT_7.2 + DPDK + GPU PRACH

```bash
mkdir -p build_gpu_split7_2 && cd build_gpu_split7_2
cmake -DDU_SPLIT_TYPE=SPLIT_7_2 \
      -DENABLE_DFT_GPU=ON \
      -DENABLE_DPDK=True \
      -DASSERT_LEVEL=MINIMAL \
      ../
make -j$(nproc)
```

Binary: `build_gpu_split7_2/apps/gnb/gnb`.

### 5.2 SPLIT_7.2 + DPDK + GPU PRACH + ACC100 LDPC

```bash
cmake -DDU_SPLIT_TYPE=SPLIT_7_2 \
      -DENABLE_DFT_GPU=ON \
      -DENABLE_DPDK=True \
      -DENABLE_PDSCH_HWACC=True \
      -DENABLE_PUSCH_HWACC=True \
      -DASSERT_LEVEL=MINIMAL \
      ../
make -j$(nproc)
```

> If CMake fails with `ENABLE_DFT_GPU=ON requires NVIDIA MathDx with cuFFTDx`,
> recheck the install path in [Section 3](#3-nvidia-mathdx-cufftdx-install).

---

## 6. Configuration

No YAML changes. Selection is via environment variable at gNB startup.

```bash
# Enable GPU PRACH detector:
export OCUDU_PRACH_DFT_BACKEND=gpu_full

# Disable (back to CPU FFTW):
unset OCUDU_PRACH_DFT_BACKEND
```

The choice is fixed at startup there is no runtime toggling. If a CUDA
device is unavailable, the runtime falls back to the CPU detector silently.

---

## 7. Startup verification

On gNB stdout:

```
PRACH DFT backend: gpu_full (CUDA-shell detector)
```

After the first PRACH window arrives (stderr):

```
[prach_detector_gpu] constructed: idft_long=1024 idft_short=256 max_batch=768 ...
[prach_detector_gpu] graph cache miss #1: long=false seq=64 ports*sym=1 dft=256 shifts=1 win_width=116 build=4953us (cached_graphs=1)
```

Every 1000 detection windows:

```
[prach_detector_gpu] stats: detects=2000 graph_builds=1 cached_graphs=1 mean=79us min=72us max=109us
[prach_detector_cpu] stats: detects=0 mean=0us min=0us max=0us
```

`graph_builds=1` (steady), `cached_graphs=1`, and CPU `detects=0` confirm
the GPU path is handling all PRACH windows.

---

## 8. Troubleshooting

| Symptom | Cause / fix |
|---|---|
| `OCUDU_PRACH_DFT_BACKEND=gpu_full but GPU detector unavailable, falling back` | No CUDA device visible, or build was without `ENABLE_DFT_GPU=ON`. Run `nvidia-smi`; rebuild if needed. |
| CMake error `ENABLE_DFT_GPU=ON requires NVIDIA MathDx with cuFFTDx` | MathDx not at `/opt/nvidia-mathdx-26.03.0-cuda13/`. Re-extract per [Section 3](#3-nvidia-mathdx-cufftdx-install). |
| `[prach_detector_cpu] stats: detects > 0` while GPU is active | A config path routed to the CPU fallback (e.g., reserved cyclic shift, unsupported DFT size). Check the surrounding logs for the config that triggered it. |
| `max` latency stays above 1 ms after window 2000 | GPU is downclocking. Apply persistence mode + clock-lock per [Section 4](#4-gpu-runtime-tuning-recommended-for-production). |
| Repeated `graph cache miss #N` lines after startup | Cell config is changing per-window (unexpected). Each unique config key pays one build. Confirm cell ZCZ / port count are stable. |

---

## 9. Deployment checklist

- [ ] NVIDIA driver ≥ 525, CUDA Toolkit ≥ 12.0 installed
- [ ] MathDx 26.03 extracted to `/opt/nvidia-mathdx-26.03.0-cuda13/`
- [ ] GPU persistence mode + clock-lock applied
- [ ] gNB built with `ENABLE_DFT_GPU=ON`
- [ ] `OCUDU_PRACH_DFT_BACKEND=gpu_full` exported in the gNB process env
- [ ] `PRACH DFT backend: gpu_full` confirmed on stdout
- [ ] `graph_builds=1` and `[prach_detector_cpu] detects=0` confirmed after 1000 detects
