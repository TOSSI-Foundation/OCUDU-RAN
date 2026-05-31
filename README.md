# OCUDU - Inline GPU Acceleration for PRACH and SRS

This is a 5G O-RAN CU/DU stack that adds an **inline-GPU uplink-PHY data path** for
PRACH preamble detection and SRS channel estimation, where the NIC DMA-writes
fronthaul packets directly into GPU VRAM via NVIDIA GPUDirect RDMA and the CPU
never sees the IQ payload.

The split lets two of the most timing-critical L1 functions - PRACH detect and
SRS estimate - run as zero-host-copy GPU pipelines on a commodity NVIDIA RTX
A4000 (sm_86) while the rest of the gNB stays on the upstream CPU path. PUSCH /
PUCCH / DL / scheduler / OFH classify-and-forward continue to use the regular
CPU OFH receiver.

The fork preserves full upstream OCUDU functionality - same L1/2/3 stack, same
3GPP and O-RAN Alliance compliance - and adds:

- **GPUDirect-RDMA mbuf pool** - per-sector VRAM region installed in the NIC's
  IOMMU domain via `rte_extmem_register` + `rte_dev_dma_map`, wrapped as a
  DPDK mbuf pool. UL packets land in GPU memory; only the 48-byte OFH header
  is peeked by a CPU poll thread to classify them.
- **Duplicate-flow rule** - two `rte_flow` entries on the same eCPRI match:
  one to NIC queue 1 (GPU VRAM, the inline path), one to queue 0 (CPU RAM,
  the stock OFH path). Non-PRACH/SRS UL still reaches the upstream CPU
  receiver; PRACH and SRS get routed to the inline pipelines.
- **Inline PRACH detector** - full 1:1 port of `prach_detector_generic_impl`
  to CUDA. BFP9 decompress is the first GPU kernel; correlation + batched
  cuFFT IDFT + power combine + GLRT argmax all run in one CUDA stream with a
  single `cudaStreamSynchronize` per detection.
- **Inline SRS estimator** - least-squares + reference correlation + cuFFT
  IDFT for the timing-advance peak + per-RE phase compensation + noise
  variance, batched as one captured CUDA graph that replays N UEs per
  occasion in a single launch.
- **Lock-free MAC ↔ GPU taps** - `srs_schedule_tap` lets the GPU listener
  classify SRS packets against the MAC-published per-slot schedule;
  `srs_result_tap` returns the GPU estimate to the upper-PHY `process_srs`,
  which attaches RNTI and suppresses the redundant CPU estimate.
- **Two deployment modes**: GPU-inline (per-cell flag `prach_rx_to_gpu` /
  `srs_rx_to_gpu`) and CPU-only - same binary, the YAML picks the path. No
  rebuild between modes.
- **Standalone microbenchmark** - `apps/examples/srs_inline_benchmark/` for
  GPU-vs-CPU SRS sweep across `#UEs × allocation_width`. A live PRACH A/B is
  driven by `scripts/prach_ab_benchmark.sh` against an `ru_emulator`.

## Topologies

The inline path always coexists with the CPU OFH receiver - the duplicate-flow
rule means *every* UL packet reaches the CPU path; only PRACH/SRS additionally
reach the GPU. Two deployment shapes are supported.

### 1. CPU-only (default)

No CUDA, no GPU; the standard upstream-OCUDU L1 path. Same binary as below
with `prach_rx_to_gpu` / `srs_rx_to_gpu` left out of the YAML.

```
                        Host
   ┌──────────────────────────────────────────────────┐
   │                                                  │
   │   ┌────────┐   eCPRI    ┌─────────────────────┐  │
   │   │  NIC   │ ─────────► │  CPU OFH receiver   │  │
   │   │ queue0 │            │  PRACH (AVX-512)    │  │
   │   └────────┘            │  SRS  (generic)     │  │
   │                         │  PUSCH/PUCCH/...    │  │
   │                         └─────────────────────┘  │
   └──────────────────────────────────────────────────┘
```

### 2. Inline-GPU PRACH + SRS

The NIC steers PRACH + SRS into a VRAM-backed mempool. A CPU poll thread
("GPU listener") drains queue 1, peeks each frame's 48 B header, and dispatches
to the matching inline pipeline. The mbuf body never leaves VRAM.

```
                                           Host
   ┌──────────────────────────────────────────────────────────────────────────┐
   │                                                                          │
   │                              NIC (mlx5_pci)                              │
   │                              ╱             ╲                             │
   │                  queue 0 (CPU RAM)     queue 1 (GPU VRAM,  GPUDirect)    │
   │                      │                       │                           │
   │                      ▼                       ▼                           │
   │             ┌─────────────────┐    ┌─────────────────────────┐           │
   │             │ CPU OFH receiver│    │ gpu_listener (CPU poll) │           │
   │             │ PUSCH / PUCCH / │    │ peek 48B header → route │           │
   │             │ everything-else │    │     PRACH/SRS/other     │           │
   │             └─────────────────┘    └──────────┬──────────────┘           │
   │                                               ▼                          │
   │                                  ┌────────────┴────────────┐             │
   │                                  ▼                         ▼             │
   │                       ┌─────────────────────┐    ┌────────────────────┐  │
   │                       │ inline_prach_pipe   │    │ inline_srs_pipe    │  │
   │                       │ (CUDA graph/detect) │    │ (CUDA graph/batch) │  │
   │                       └──────────┬──────────┘    └──────────┬─────────┘  │
   │                                  │                          │            │
   │                                  ▼                          ▼            │
   │                              MAC PRACH                upper-PHY          │
   │                              callback                 process_srs        │
   │                                                       (via srs_result_   │
   │                                                        tap; RNTI attach) │
   │                                                                          │
   └──────────────────────────────────────────────────────────────────────────┘
```

## Quick start

### Build (GPU-inline)

```bash
cd ~/ocudu
rm -rf build_72
mkdir build_72 && cd build_72

cmake .. \
  -DDU_SPLIT_TYPE=SPLIT_7_2 \
  -DENABLE_DPDK=True \
  -DENABLE_GPU_FRONTHAUL=ON \
  -DASSERT_LEVEL=MINIMAL \
  -DENABLE_UHD=OFF \
  -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_CUDA_ARCHITECTURES=86

make gnb ru_emulator srs_inline_benchmark -j$(nproc)
```

Set `-DCMAKE_CUDA_ARCHITECTURES=` to your GPU's compute capability: `86`
(A4000 / RTX 30-series), `80` (A100), `89` (Ada / RTX 4090), `90` (H100).

### Build (CPU-only - no CUDA, no GPU)

```bash
cd ~/ocudu
rm -rf build_cpu
mkdir build_cpu && cd build_cpu

cmake .. \
  -DDU_SPLIT_TYPE=SPLIT_7_2 \
  -DENABLE_DPDK=True \
  -DENABLE_GPU_FRONTHAUL=OFF \
  -DASSERT_LEVEL=MINIMAL \
  -DENABLE_UHD=OFF \
  -DCMAKE_BUILD_TYPE=Release

make gnb -j$(nproc)
```

Same source tree. Drop `-DENABLE_DPDK=True` too if you're driving the RU over
sockets instead of DPDK.

### Run - inline-GPU pipeline

```bash
# Terminal 1: start the RU emulator (synthetic UL traffic with PRACH + optional SRS replay)
sudo ./build_72/apps/examples/ofh/ru_emulator -c configs/emu.yml

# Terminal 2: start the gNB with the GPU-inline YAML
sudo ./build_72/apps/gnb/gnb -c configs/gpu_inline_acc_gnb.yml
```

Expect, in the gnb startup log:

```
[gpu_init]    CUDA driver=… runtime=… device='NVIDIA RTX A4000' sm_86 …
[gpu_init]    DPDK gpudev backend devs=1 (using dev_id=0) …
[ofh_factories] Sector#0 inline GPU PRACH pipeline active (nof_prach_eaxc=4 …)
[ofh_factories] Sector#0 inline GPU SRS  pipeline active (nof_rx_ports=4 …)
DPDK GPU - port 0 link=UP speed=… duplex=full …
[gpu_listener] thread started: port=0 queue=1 prach_eaxcs_count=4
```

The two `pipeline active` lines render bold-green on a TTY (TTY-aware via
`isatty()` - plain text when redirected to a file).

### Run - PRACH live A/B benchmark

```bash
# With ru_emulator running:
sudo ./scripts/prach_ab_benchmark.sh 100 12
```

The script restarts the gnb twice, flipping only `prach_rx_to_gpu`
(true → false) between runs, samples for 100 s after a 12 s warmup, parses
the per-window stats from both gnb logs, and prints a side-by-side table:
`detector mean latency`, `min/max`, `stats windows`, `detections logged`.
The benchmark exports `OCUDU_PRACH_BENCH=1` automatically so the stats lines
are emitted; interactive gnb runs stay quiet by default.

### Run - SRS standalone microbenchmark

```bash
sudo ./build_72/apps/examples/srs_inline_benchmark/srs_inline_benchmark 5000
```

Sweeps `#UEs ∈ {1, 2, 4, 8, 16, 32, 64, 128, 256}` at three allocation widths
(seq_len 12 / 192 / 552 → 4 / 64 / 184 PRB), reports CPU vs GPU per-occasion
microseconds and the GPU-graph speedup. No NIC, no gnb, no root strictly
needed (sudo is used only because the dev box's CUDA context is locked
EXCLUSIVE_PROCESS).

## Configuration

Two YAML configs cover both topologies. The same gnb binary serves both
deployments; the only difference is whether the per-cell `prach_rx_to_gpu` /
`srs_rx_to_gpu` flags are present.

| File | Role |
|---|---|
| `configs/gpu_inline_acc_gnb.yml` | GNB. GPU-inline. mlx5 NIC + GPUDirect-RDMA mempool. `prach_rx_to_gpu: true` + `srs_rx_to_gpu: true` per cell. |
| `configs/emu.yml`          | `ru_emulator`. PRACH-replay-only synthetic UL. |
| `configs/emu_srs_replay.yml` | `ru_emulator` with `srs_replay_file: /tmp/srs.bin` set - splices captured SRS IQ onto UL U-plane on `(sf=1, slot=1, sym=13)` so the inline SRS estimator can be exercised without a real UE. |

### Key YAML knobs

**`ru_ofh.cells[i]`** (per cell):

- `prach_rx_to_gpu: true|false` - route eCPRI to a GPU-backed RX queue and
  classify PRACH eAxCs in the GPU listener.
- `srs_rx_to_gpu: true|false` - additionally classify SRS via
  `srs_schedule_tap` and run the inline GPU estimator. Requires
  `prach_rx_to_gpu: true` (shares the listener + duplicate-rule plumbing).

**`cell_cfg.prach`** - drives the inline-PRACH pipeline's per-cell config
(`prach_config_index`, `zero_correlation_zone`, `prach_root_sequence_index`,
`restricted_set`). Standard upstream block - no inline-specific knobs.

**`cell_cfg.srs`** - drives the inline-SRS pipeline's per-cell shape
(`type_enabled`, `period_ms`, `max_nof_sym_per_slot`, `tx_comb`, etc.).

**`dpdk.eal_args`** - required `--huge-dir`, `--file-prefix=gnb` (matches the
gnb's `mp_socket` path the startup banner reports), `--iova-mode=pa`, and
`-a <PCIe>` for the NIC.

### Runtime environment variables

| Variable | Purpose |
|---|---|
| `OCUDU_PRACH_BENCH=1` | Unsuppresses periodic GPU listener / detector stats (`[gpu_listener] …`, `[prach_detector_inline] stats:`, `[prach_detector_cpu] stats:`). Default off; the benchmark script sets it automatically. |
| `OCUDU_SRS_GPU_FORCE=<sf>:<slot>:<sym>` | Force the GPU listener to treat every packet at the given subframe/slot/symbol as SRS, regardless of `srs_schedule_tap`. Used for UE-less rig validation. |
| `OCUDU_SRS_GPU_RES=csrs:bsrs:fpos:fshift:comb:coff:seqid:nsym:nrx` | When set with `OCUDU_SRS_GPU_FORCE`, publishes a synthetic SRS resource configuration matching a captured-IQ payload. |
| `OCUDU_PRACH_DFT_BACKEND` | Selects the *earlier* CUDA-graph PRACH backend (`gpu_full`). Leave unset / `cpu` when running the inline path - the two are mutually exclusive. |

## Performance (validated on RTX A4000, 100 MHz 4T4R cell)

### PRACH detect - live A/B against `ru_emulator`

| Metric | Inline GPU | Default CPU |
|---|---:|---:|
| Mean detector latency | ~108 µs | ~325 µs |
| Speedup | **~3.0×** | (baseline) |
| Latency floor (min) | 44 µs | 320 µs |
| Tail (max) | 2.5 ms (rare) | 770 µs |

### SRS estimate - captured-graph throughput sweep (4 RX, 64 PRB)

| #UEs | CPU / occ | GPU graph / occ | Speedup |
|---:|---:|---:|---:|
|   1 | 13.5 µs | 44.1 µs |  0.31× |
|   4 | 13.5 µs | 11.0 µs |  1.23× |
|  16 | 13.5 µs |  2.7 µs |  5.06× |
|  64 | 13.5 µs | 0.84 µs |  16.0× |
| 256 | 13.5 µs | 0.59 µs | **22.8×** |

Crossover (GPU > CPU) drops as allocation widens: ~16 UEs at 4 PRB, ~4 UEs
at 64 PRB, just ~2 UEs at 184 PRB. Full performance methodology + 184-PRB


## Repository layout

| Path | Contents |
|---|---|
| `include/ocudu/hal/cuda/`  | Public headers - `inline_prach_pipeline.h`, `inline_srs_pipeline.h`, `prach_detector_inline.h`, `srs_estimator_inline.h`, `vram_prach_buffer.h`, `vram_srs_buffer.h`, `gpu_dpdk_mempool.h`, `dpdk_gpu_ethernet_receiver.h`, `prach_flow_rules.h`. |
| `include/ocudu/support/`   | Lock-free MAC↔GPU rings - `srs_schedule_tap.h` (MAC→listener) and `srs_result_tap.h` (GPU→upper PHY). |
| `lib/hal/cuda/`            | Inline pipelines + kernels - `inline_prach_pipeline.cpp`, `prach_detector_inline_impl.cpp`, `prach_detector_inline_kernels.cu`, `prach_inline_bfp_kernel.cu`, `inline_srs_pipeline.cpp`, `srs_estimator_inline_impl.cpp`, `srs_inline_estimator_kernels.cu`, `srs_inline_bfp_kernel.cu`, `vram_prach_buffer.cpp`, `vram_srs_buffer.cpp`, `gpu_dpdk_mempool.cpp`, `gpu_rx_buffer.cpp`, `dpdk_gpu_ethernet_receiver.cpp`, `prach_flow_rules.cpp`. |
| `lib/ofh/ethernet/dpdk/`   | DPDK port context + dual-queue init + GPU listener thread + `--log-level` injection - `dpdk_ethernet_port_context.cpp`, `dpdk_ethernet_factories.cpp`. |
| `lib/ofh/`                 | Factory wire-through that constructs the inline pipelines per sector based on the YAML flags - `ofh_factories.cpp`. |
| `lib/phy/upper/`           | Upper-PHY consumer of the SRS result tap (`uplink_processor_impl.cpp`) and the gated CPU PRACH detector (`channel_processors/prach/prach_detector_generic_impl.*`). |
| `lib/fapi_adaptor/phy/p7/` | MAC-side `srs_schedule_tap::publish` from the FAPI fastpath translator. |
| `apps/examples/srs_inline_benchmark/` | Standalone CPU-vs-GPU SRS microbenchmark binary. |
| `apps/examples/ofh/ru_emulator*` | RU emulator with synthetic PRACH transmission and optional SRS-IQ replay (`--srs_replay_file`). |
| `configs/`                 | `gpu_inline_acc_gnb.yml` (gNB GPU-inline), `emu.yml` + `emu_srs_replay.yml` (ru_emulator). |
| `scripts/prach_ab_benchmark.sh` | Live PRACH GPU vs CPU benchmark driver. |

## License

BSD 3-Clause Open MPI variant. See `LICENSE`. Portions of this software may
implement 3GPP specifications, which may be subject to additional licensing
requirements.
