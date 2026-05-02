<!--
SPDX-FileCopyrightText: Copyright (C) 2021-2026 Software Radio Systems Limited
SPDX-License-Identifier: BSD-3-Clause-Open-MPI
-->

# OCUDU - GPU PRACH-Accelerated Stack


<img src="https://srs.io/wp-content/uploads/ocudu_color.png" alt="image" width="50%"/>

This is a 5G O-RAN CU/DU stack that adds **GPU-accelerated PRACH preamble detection on NVIDIA Ampere-class GPUs**, dispatched through a fused cuFFTDx kernel chain captured into a CUDA graph. The accelerator path replaces the AVX-512 FFTW software detector for the PRACH inner loop (correlation, IDFT, power accumulation, peak detection), while everything else in the upper-PHY chain stays on the CPU.

The fork preserves full upstream OCUDU functionality - the same L1/2/3 stack, the same 3GPP and O-RAN Alliance compliance and adds:

- **PRACH detection offload** via cuFFTDx fused kernel + CUDA graph → NVIDIA GPU silicon
- **Unified PHY metrics** instrumenting both software and HW paths with identical fields (direct A/B comparison from one log format)
- **Zero-allocation hot path** - all device + pinned-host buffers pre-allocated at construction; `detect()` is a single `cudaGraphLaunch + cudaStreamSynchronize`
- **Configuration-driven activation** - flip on/off via the `OCUDU_PRACH_DFT_BACKEND` env var, no code changes, no YAML required

---

## Measured impact (real-traffic A/B)

Single-cell short-format PRACH (B4), seq=64, ports×sym=1, DFT=256. Live gNB with real over-the-air UEs. **Same host, same RU; only the env var differs.** Mean / max over rolling 1000-detect windows in steady state:

| Metric | CPU FFTW | GPU cuFFTDx | Δ |
|---|---|---|---|
| **Split 8 / X410 1T1R** mean latency | 112 µs | 78 µs | **−30 %** |
| Split 8 / X410 1T1R tail latency (max) | 161 µs | 107 µs | **−33 %** |
| **Split 7.2 / Liteon 4T4R** mean latency | 115 µs | 74 µs | **−36 %** |
| Split 7.2 / Liteon 4T4R tail latency (max) | 187 µs | 87 µs | **−55 % (2.1× tighter)** |
| Cold-start CUDA graph build (one-time, pre-UE-attach) | — | 3.5–5 ms | one per config key |

The tail-latency compression is the headline operational benefit: on Liteon 4T4R the worst-case PRACH detect drops by more than half, leaving margin in the slot deadline for everything downstream of the detector.

---

## Architecture at a glance

```
┌────────────────────────────────────────────────────────────────────┐
│ DU-LOW · Upper PHY                                                 │
│                                                                    │
│  PRACH:  OFH RX → OFDM demod → PRACH buffer → ★ PRACH DETECT ★ → MAC│
│                                                       │            │
└───────────────────────────────────────────────────────┼────────────┘
                                                       ▼
                                  ┌─────────────────────────────────┐
                                  │  prach_detector_gpu_impl        │
                                  │  (CPU prologue: RSSI, ZC cache, │
                                  │   stage cbf16, graph lookup)    │
                                  ├─────────────────────────────────┤
                                  │  CUDA Graph Exec (cached)       │
                                  │   H2D → cbf16→cf32 → cuFFTDx    │
                                  │   → combine → per-shift accum   │
                                  │   → argmax+divide → D2H         │
                                  ├─────────────────────────────────┤
                                  │  cuFFT / cuFFTDx / CUDA runtime │
                                  ├─────────────────────────────────┤
                                  │  NVIDIA GPU (RTX A4000 / 8000)  │
                                  └─────────────────────────────────┘
                                            via PCIe / pinned host
```

PRACH detect is the only stage that moves off-CPU; the upper-PHY factory selects GPU or CPU once at startup based on the `OCUDU_PRACH_DFT_BACKEND` environment variable.

---

## Documentation

| Document | Contents |
|---|---|
| **[`docs/DEPLOYMENT.md`](docs/DEPLOYMENT.md)** | Full deployment guide: prerequisites, NVIDIA driver / CUDA / MathDx install, GPU persistence and clock-lock setup, build flags, env-var configuration, startup verification, kernel-chain reference, metrics, results, troubleshooting |

---

## Quick start (GPU PRACH path)

Prerequisites in one line: NVIDIA Ampere GPU (SM86+), CUDA Toolkit ≥ 12.0, NVIDIA MathDx 26.03 at `/opt/nvidia-mathdx-26.03.0-cuda13/`, NVIDIA driver ≥ 525. Full prerequisite + setup procedure in [`docs/DEPLOYMENT.md`](docs/DEPLOYMENT.md).

```bash
# Build
mkdir -p build_gpu_split7_2 && cd build_gpu_split7_2
cmake -DDU_SPLIT_TYPE=SPLIT_7_2 \
      -DENABLE_DFT_GPU=ON \
      -DENABLE_DPDK=True \
      -DASSERT_LEVEL=MINIMAL \
      ../
make -j$(nproc)

# Run with GPU PRACH detector
OCUDU_PRACH_DFT_BACKEND=gpu_full \
  ./apps/gnb/gnb -c /path/to/gnb_split7_2.yaml
```

Look for these lines in the log to confirm the GPU detector is in use:

```
PRACH DFT backend: gpu_full (CUDA-shell detector)
[prach_detector_gpu] constructed: idft_long=1024 idft_short=256 max_batch=768 ...
[prach_detector_gpu] graph cache miss #1: long=false seq=64 ports*sym=1 dft=256 ... build=4953us
[prach_detector_gpu] stats: detects=1000 graph_builds=1 cached_graphs=1 mean=78us min=70us max=107us
```

The CPU fallback detector's stats line should show `detects=0` throughout the run, confirming all PRACH windows are routed to the GPU path.

---

## Repository layout

| Path | Contents |
|---|---|
| [`apps/`](apps/) | Application units (gNB, DU, CU, split-7.2 / split-8 helpers) |
| [`lib/hal/cuda/`](lib/hal/cuda/) | CUDA HAL: **`prach_detector_gpu_kernel.cu`** (six per-stage kernels) and **`prach_detector_gpudx_kernel.cu`** (cuFFTDx fused kernel for sizes 128–4096) |
| [`lib/phy/upper/channel_processors/prach/`](lib/phy/upper/channel_processors/prach/) | PRACH detector implementations; **`prach_detector_gpu_impl.cpp`** drives the CUDA graph pipeline and CPU bookends |
| [`lib/phy/metrics/`](lib/phy/metrics/) | PHY metrics, including the unified GPU/CPU detector stats |
| [`lib/du/du_low/`](lib/du/du_low/) | DU-low (PHY) factory and runtime; reads `OCUDU_PRACH_DFT_BACKEND` to choose the detector |
| [`lib/du/du_high/`](lib/du/du_high/) | DU-high (MAC, RLC, scheduler) |
| [`lib/cu_cp/`](lib/cu_cp/) · [`lib/cu_up/`](lib/cu_up/) | CU control plane and user plane |
| [`docs/`](docs/) | Project documentation (this fork's deployment + integration guides) |

---

## License

BSD 3-Clause Open MPI variant - see the [LICENSE](./LICENSE) file. Portions of this software may implement 3GPP specifications, which may be subject to additional licensing requirements.
