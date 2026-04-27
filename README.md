<!--
SPDX-FileCopyrightText: Copyright (C) 2021-2026 Software Radio Systems Limited
SPDX-License-Identifier: BSD-3-Clause-Open-MPI
-->

# OCUDU  ACC100 Hardware-Accelerated Stack


<img src="https://srs.io/wp-content/uploads/ocudu_color.png" alt="image" width="50%"/>

This is 5G O-RAN CU/DU stack that adds **hardware-accelerated LDPC encoding and decoding on Intel ACC100 vRAN accelerator cards**, dispatched through the DPDK BBDEV API. The accelerator path replaces the AVX2/AVX-512 software kernels for the two heaviest channel-coding stages (PDSCH encode, PUSCH decode), while everything else in the upper-PHY chain stays on the CPU.

The fork preserves full upstream OCUDU functionality  the same L1/2/3 stack, the same 3GPP and O-RAN Alliance compliance  and adds:

- **LDPC offload for PDSCH and PUSCH** via DPDK BBDEV → ACC100 silicon
- **Unified PHY metrics** instrumenting both software and HW paths with identical fields (direct A/B comparison from one log format)
- **Multi-VF scaling** out of the box; allowlist additional ACC100 VFs and load spreads automatically
- **Configuration-driven activation**  flip on/off via DU YAML, no code changes

---

## Measured impact (real-traffic A/B)

Single-cell n78 100 MHz TDD, 4T4R, real UE + iperf3 + video + speed tests. **Same UE, same RU; only the gNB binary differs.** Mean values over ~175 one-second windows:

| Metric | AVX-512 software | ACC100 hardware | Δ |
|---|---|---|---|
| LDPC Decoder avg latency | 51.6 µs | 24.5 µs | **−53 % (2.1× faster)** |
| LDPC Decoder tail latency (max) | 145.9 µs | 40.5 µs | **−72 % (3.6× tighter)** |
| LDPC Decoder throughput | 46.6 Mbps | 137.4 Mbps | **+195 % (2.9×)** |
| PDSCH Processor proc rate | 198.0 Mbps | 310.1 Mbps | **+57 %** |
| PUSCH Processor proc rate | 24.0 Mbps | 37.4 Mbps | **+56 %** |
| **Upper-PHY UL CPU usage** | **3.79 %** | **2.36 %** | **−38 %** |

The uplink CPU reduction is the headline operational benefit: roughly **one core freed** under sustained traffic, available for higher cell counts on the same host or tighter scheduling-latency budgets.

---

## Architecture at a glance

```
┌────────────────────────────────────────────────────────────────────┐
│ DU-LOW · Upper PHY                                                 │
│                                                                    │
│  PDSCH:  TB → segment → ★ LDPC ENC ★ → rate-match → … → OFH TX     │
│  PUSCH:  OFH RX → demod → rate-dematch → ★ LDPC DEC ★ → CRC → MAC  │
│                              │                                     │
└──────────────────────────────┼─────────────────────────────────────┘
                               ▼
                  ┌─────────────────────────┐
                  │  HWACC metric decorator │   (unified metrics tap)
                  ├─────────────────────────┤
                  │  pdsch_enc_bbdev_impl   │
                  │  pusch_dec_bbdev_impl   │   (HARQ context, batching)
                  ├─────────────────────────┤
                  │  DPDK BBDEV device      │   (mbuf + op pools)
                  ├─────────────────────────┤
                  │  ACC100 silicon         │   (LDPC enc + dec + HARQ)
                  └─────────────────────────┘
                            via PCIe / VFIO
```

LDPC encode and LDPC decode are the only stages that move off-CPU; the upper-PHY factory selects HW or software once at startup based on the YAML.

---

## Documentation

| Document | Contents |
|---|---|
| **[`docs/DEPLOYMENT.md`](docs/DEPLOYMENT.md)** | Full deployment guide: prerequisites, kernel/VFIO setup, DPDK and pf_bb_config install, build flags, YAML configuration, startup verification, metrics, troubleshooting |

---

## Quick start (ACC100 path)

Prerequisites in one line: Intel ACC100 with at least one VF, DPDK ≥ 22.11, `pf_bb_config` ≥ 24.03, IOMMU on, hugepages allocated. Full prerequisite + setup procedure in [`docs/DEPLOYMENT.md`](docs/DEPLOYMENT.md).

```bash
# Build
mkdir -p build_hwacc && cd build_hwacc
sudo cmake -DDU_SPLIT_TYPE=SPLIT_7_2 \
           -DENABLE_DPDK=True \
           -DENABLE_PDSCH_HWACC=True \
           -DENABLE_PUSCH_HWACC=True \
           -DASSERT_LEVEL=MINIMAL \
           ../
sudo make -j$(nproc)

# Run
sudo ./apps/gnb/gnb -c /path/to/gnb_acc100.yaml
```

Look for these lines in the log to confirm the accelerator is in use:

```
[HWACC] [I] [bbdev] dev=0 driver=intel_acc100_vf ...
[HWACC] [I] [bbdev] dev=0 started: ldpc_enc_q=N ldpc_dec_q=N ...
```

---

## Repository layout

| Path | Contents |
|---|---|
| [`apps/`](apps/) | Application units (gNB, DU, CU, split-7.2 / split-8 helpers) |
| [`lib/hal/dpdk/`](lib/hal/dpdk/) | DPDK EAL, mbuf pools, **BBDEV device wrappers** (`bbdev_acc.cpp`, op pool factory) |
| [`lib/phy/upper/channel_processors/`](lib/phy/upper/channel_processors/) | Upper-PHY processors; **HW LDPC encode/decode BBDEV impls** live here |
| [`lib/phy/metrics/`](lib/phy/metrics/) | PHY metrics, including the **HWACC decorator** layer |
| [`lib/du/du_low/`](lib/du/du_low/) | DU-low (PHY) factory and runtime |
| [`lib/du/du_high/`](lib/du/du_high/) | DU-high (MAC, RLC, scheduler) |
| [`lib/cu_cp/`](lib/cu_cp/) · [`lib/cu_up/`](lib/cu_up/) | CU control plane and user plane |
| [`docs/`](docs/) | Project documentation (this fork's deployment + integration guides) |

---

## License

BSD 3-Clause Open MPI variant  see the [LICENSE](./LICENSE) file. Portions of this software may implement 3GPP specifications, which may be subject to additional licensing requirements.
