<!--
SPDX-FileCopyrightText: Copyright (C) 2021-2026 Software Radio Systems Limited
SPDX-License-Identifier: BSD-3-Clause-Open-MPI
-->

# OCUDU - FAPI L1/L2 Split with XFAPI Bridge

This is a 5G O-RAN CU/DU stack that adds an **in-process FAPI split between L2 (MAC) and L1 (PHY)**, with messages flowing through an **xSM (DPDK-backed shared-memory) transport** and an **XFAPI translator-bridge** sitting between the two processes.

The split lets `odu_high` (L2/MAC) and `odu_low` (L1/PHY) run as separate processes, optionally on separate hosts, while remaining wire-compatible with the upstream OCUDU FAPI implementation.

The fork preserves full upstream OCUDU functionality: the same L1/2/3 stack, the same 3GPP and O-RAN Alliance compliance, and adds:

- **FAPI L1/L2 split** through a serialized P5/P7 transport over xSM shared memory
- **XFAPI translator-bridge** that decouples L1 and L2 endpoints, allowing them to live in different DPDK domains or on different hosts
- **Two deployment topologies**: single-host (all three processes on one machine) and two-host XFAPI split (L1 + XFAPI-L1 on one server, XFAPI-L2 + L2 on the other, connected by a dedicated DPDK-Ethernet link)
- **Configuration-driven activation** through YAML (`fapi_split_l1` / `fapi_split_l2` blocks); same binary serves both monolithic and split deployments

---

## Topologies

The FAPI split always runs with XFAPI as the translator-bridge between `odu_low` and `odu_high`. Two deployments are supported.

### 1. Single-host bridge

All three processes run on the same server, sharing one xSM memzone with two slot pairs.

```text
                          Server A
   ┌────────────────────────────────────────────────────────┐
   │                                                        │
   │   ┌──────────┐   pair 0   ┌─────────┐   pair 1   ┌──────────┐
   │   │ odu_low  │ ◄────────► │  XFAPI  │ ◄────────► │ odu_high │
   │   │ PRIMARY  │            │ SECOND. │            │ SECOND.  │
   │   │ SLAVE    │            │ MASTER+ │            │ MASTER   │
   │   │          │            │ SLAVE   │            │          │
   │   └──────────┘            └─────────┘            └──────────┘
   │                                                        │
   │             shared xSM memzone "xsm_bridge"            │
   │                 (DPDK file-prefix=gnb0)                │
   └────────────────────────────────────────────────────────┘
```

### 2. Two-host XFAPI split

L1 and L2 run on different servers connected by a dedicated DPDK-Ethernet link between the two XFAPI instances.

```text
            Server A (L1 host)                              Server B (L2 host)
   ┌────────────────────────────────┐              ┌────────────────────────────────┐
   │  ┌──────────┐  ┌──────────┐    │   DPDK-Eth   │    ┌──────────┐  ┌──────────┐  │
   │  │ odu_low  │◄►│ XFAPI-L1 │◄───┼──────────────┼───►│ XFAPI-L2 │◄►│ odu_high │  │
   │  │ PRIMARY  │  │ SECOND.  │    │ (wire link)  │    │ PRIMARY  │  │ SECOND.  │  │
   │  │ SLAVE    │  │ MASTER + │    │              │    │ SLAVE +  │  │ MASTER   │  │
   │  │          │  │ Eth-MSTR │    │              │    │ Eth-MSTR │  │          │  │
   │  └──────────┘  └──────────┘    │              │    └──────────┘  └──────────┘  │
   │  file-prefix = gnb0            │              │    file-prefix = gnb0_l2       │
   └────────────────────────────────┘              └────────────────────────────────┘
```

### 3. OAI L1 over the bridge (disaggregated, two-host)

The L1 endpoint is an **OAI L1** instead of OCUDU's `odu_low`, and the deployment is disaggregated across two hosts:

- **Host A — OAI L1 alone**: OAI `nr-softmodem` (openair1 PHY) driving the Liteon RU over fronthaul, exposed as an **nFAPI PNF** (`NFAPI_MODE=PNF`). It speaks **open-nFAPI** over the Linux kernel net stack — **P5 over SCTP** (client) and **P7 over UDP** (bidirectional, big-endian TLV).
- **Host B — xFAPI + OCUDU DU-High co-located**: xFAPI runs in **OAI_OCUDU mode** as the nFAPI **VNF** (P5 SCTP listener + P7 UDP listener), translating nFAPI ↔ FAPI and bridging it onto **xSM shared memory** for `odu_high`. OCUDU DU-High is the xSM **master on pair 1** (DPDK secondary).

So the wire is **nFAPI (SCTP/UDP) between Host A and Host B**, and **xSM (shared memory) between xFAPI and OCUDU inside Host B**.

```text
        Host A: OAI L1                          Host B: xFAPI + OCUDU DU-High
   ┌───────────────────────┐                ┌──────────────────────────────────────────┐
   │  ┌─────────────────┐  │    nFAPI       │  ┌─────────┐  xSM pair 1   ┌────────────┐ │
   │  │     OAI L1      │  │  P5 SCTP +     │  │  xFAPI  │ ◄───────────► │  OCUDU     │ │
   │  │ nr-softmodem    │◄─┼──P7 UDP────────┼─►│OAI_OCUDU│  shared mem   │  DU-High   │ │   F1AP
   │  │ nFAPI PNF       │  │ (kernel stack) │  │  mode   │  "xsm_bridge" │ (odu_high) │ │ ◄──────► CU
   │  │ + Liteon RU     │  │  big-endian    │  │ nFAPI   │   m2s / s2m   │ xSM MASTER │ │
   │  └─────────────────┘  │     TLV        │  │  VNF    │   zero-copy   │ pair 1     │ │
   └───────────────────────┘                │  └─────────┘               └────────────┘ │
                                            │   DPDK secondary · file-prefix gnb0_l2     │
                                            └──────────────────────────────────────────┘
```

`odu_high` runs with [`configs/ocudu_xfapi_oai.yaml`](configs/ocudu_xfapi_oai.yaml) (OAI-tuned `cell_cfg`, same `fapi_split_l2` transport). See [OAI L1 interop](#oai-l1-interop) below for the run steps.

Full topology details, startup order, and DPDK role tables are in [`docs/fapi_split_topologies.md`](docs/fapi_split_topologies.md).

---

## Quick start

Both `odu_low` and `odu_high` must come from this repo and use the XFAPI bridge configs in `configs/`.

### Build

```bash
./build.sh
```

This wraps the CMake configure + parallel make. Defaults: `DU_SPLIT_TYPE=SPLIT_7_2`, `ENABLE_DPDK=True`, `ASSERT_LEVEL=MINIMAL`. Edit `build.sh` if you need different flags. The resulting binaries are in `build/apps/du/odu` and `build/apps/du_low/odu_low`.

### Single-host bridge

```bash
# Terminal 1: start L1 (creates the shared xSM memzone)
./build/apps/du_low/odu_low -c configs/odu_low_xfapi.yaml

# Terminal 2: start XFAPI bridge (attaches to both pairs)
xfapi_main --cfgfile <path-to-xfapi>/conf/ocudu_ocudu_config.yaml

# Terminal 3: start L2 (attaches as master on pair 1)
./build/apps/du/odu -c configs/odu_high_xfapi.yaml
```

### Two-host XFAPI split

On the L1 host:
```bash
./build/apps/du_low/odu_low -c configs/odu_low_xfapi.yaml
xfapi_main --cfgfile <path-to-xfapi>/conf/ocudu_ocudu_split_l1.yaml
```

On the L2 host:
```bash
xfapi_main --cfgfile <path-to-xfapi>/conf/ocudu_ocudu_split_l2.yaml
./build/apps/du/odu -c configs/odu_high_xfapi.yaml
```

The L1 host must have the XFAPI link NIC bound to `vfio-pci` and listed in `odu_low_xfapi.yaml`'s `hal.eal_args` (`-a <NIC_PCI_BDF>`) so XFAPI-L1 (DPDK secondary) inherits visibility of it.

---

## Configuration

Two YAML configs cover both topologies. Only the XFAPI config file and a few L2-side knobs (`xsm_pair_index`, `dpdk_proc_type`, `xsm_file_prefix`) select between single-host and two-host deployments.

| File | Role |
|---|---|
| [`configs/odu_low_xfapi.yaml`](configs/odu_low_xfapi.yaml) | L1/PHY. DPDK primary, xSM slave on pair 0. Owns the radio (HAL, expert_phy, ru_ofh). |
| [`configs/odu_high_xfapi.yaml`](configs/odu_high_xfapi.yaml) | L2/MAC. DPDK secondary, xSM master. Owns F1AP, F1U, scheduler, cell_cfg. |
| [`configs/ocudu_xfapi_oai.yaml`](configs/ocudu_xfapi_oai.yaml) | L2/MAC variant tuned to pair `odu_high` with an **OAI L1** over the bridge. Same `fapi_split_l2` transport as above, OAI-friendly `cell_cfg`. |

Key knobs:

- `fapi_split_l1` (odu_low): `xsm_device_name`, `dpdk_proc_type`, `xsm_pair_index`, `xsm_num_pairs`, `rx_cpu`, `rx_priority`
- `fapi_split_l2` (odu_high): `xsm_device_name`, `xsm_pair_index`, `xsm_file_prefix`, `dpdk_proc_type`, `rx_cpu`, `rx_priority`
- `fapi_stats` (both): optional in-memory FAPI message recorder, dumped as JSON at shutdown

---

## OAI L1 interop

The same `odu_high` (L2) can drive an **OAI L1** instead of OCUDU's own `odu_low`. OAI L1 speaks **nFAPI** (open-nFAPI: P5 over SCTP, P7 over UDP) as an nFAPI **PNF**; xFAPI runs in **OAI_OCUDU mode** as the nFAPI **VNF** and translates nFAPI ↔ FAPI onto **xSM** for `odu_high`. Use [`configs/ocudu_xfapi_oai.yaml`](configs/ocudu_xfapi_oai.yaml): it carries the standard `fapi_split_l2` transport block plus a `cell_cfg` tuned for OAI's L1 (n78, 100 MHz, TDD), so a UE attaches end-to-end.

See the [disaggregated OAI L1 topology](#3-oai-l1-over-the-bridge-disaggregated-two-host) above for the host layout.

```bash
# Host A: OAI L1 as nFAPI PNF (P5 SCTP client -> xFAPI :50001, P7 UDP 50010<->50011)
NFAPI_MODE=PNF nr-softmodem -O <oai-gnb.conf> --rfsim   # or live RU

# Host B, Terminal 1: xFAPI bridge in OAI_OCUDU mode (nFAPI VNF <-> xSM master/slave)
xfapi_main --cfgfile <path-to-xfapi>/conf/oai_ocudu_config.yaml

# Host B, Terminal 2: OCUDU L2, OAI-tuned config (xSM master on pair 1)
./build/apps/du/odu -c configs/ocudu_xfapi_oai.yaml
```

The Host-B L2-side transport knobs (`xsm_pair_index`, `xsm_file_prefix`, `dpdk_proc_type`) must match xFAPI's xSM side (`file-prefix = gnb0_l2`, pair 1). On Host A, the OAI nFAPI P5/P7 socket endpoints must match xFAPI's nFAPI VNF listeners.

---

## Documentation

| Document | Contents |
|---|---|
| [`docs/fapi_split_topologies.md`](docs/fapi_split_topologies.md) | Supported topologies, startup order, DPDK roles and file-prefix layout, when to use each topology. |

---

## Repository layout

| Path | Contents |
|---|---|
| [`apps/du/`](apps/du/) | `odu` (monolithic) and `odu_high` (L2/MAC) entry points, FAPI xSM transport, FAPI stats recorder. |
| [`apps/du_low/`](apps/du_low/) | `odu_low` (L1/PHY) entry point. |
| [`apps/units/flexible_o_du/split_6/`](apps/units/flexible_o_du/split_6/) | Split-6 application units; xSM plugin variants selected when `ENABLE_XSM_FAPI_SPLIT=ON`. |
| [`lib/fapi/serialization/`](lib/fapi/serialization/) | FAPI P5/P7 message serialization for the xSM wire format. |
| [`lib/ipc/xsm/`](lib/ipc/xsm/) | xSM context wrapper around the precompiled `libxsm.so`. |
| [`include/ocudu/xsm/`](include/ocudu/xsm/) | Imported xSM library headers and `libxsm.so` (precompiled, no source build). |
| [`lib/support/`](lib/support/) | FAPI split trace logger, MAC/PHY handoff timing recorder, latency injector. |
| [`configs/`](configs/) | XFAPI-bridge YAML configs for L1 and L2. |
| [`docs/`](docs/) | Project documentation. |

---

## License

BSD 3-Clause Open MPI variant. See the [LICENSE](./LICENSE) file. Portions of this software may implement 3GPP specifications, which may be subject to additional licensing requirements.
