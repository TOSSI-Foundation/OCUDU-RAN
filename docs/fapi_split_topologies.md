# FAPI Split: Supported Topologies

The FAPI L1/L2 split in this release always runs with **XFAPI** as the translator-bridge between `odu_low` (L1/PHY) and `odu_high` (L2/MAC). Two deployments are supported.

## 1. Single-host bridge

All three processes run on the same server and exchange FAPI messages through one shared xSM memzone (`xsm_bridge`) with two slot pairs.

```text
                           Server A
   ┌────────────────────────────────────────────────────────┐
   │                                                        │
   │   ┌──────────┐    pair 0   ┌─────────┐    pair 1   ┌──────────┐
   │   │ odu_low  │ ◄─────────► │  XFAPI  │ ◄─────────► │ odu_high │
   │   │          │             │         │             │          │
   │   │ PRIMARY  │             │ SECOND. │             │ SECOND.  │
   │   │ SLAVE    │             │ MASTER  │             │ MASTER   │
   │   │          │             │   +     │             │          │
   │   │          │             │ SLAVE   │             │          │
   │   └──────────┘             └─────────┘             └──────────┘
   │                                                        │
   │              shared xSM memzone "xsm_bridge"           │
   │                  (DPDK file-prefix=gnb0)               │
   └────────────────────────────────────────────────────────┘
```

**Startup order** (DPDK secondary processes need the primary's memzone to exist first):

1. `odu_low  -c configs/odu_low_xfapi.yaml`    (creates the memzone)
2. `xfapi_main --cfgfile conf/ocudu_ocudu_config.yaml`   (attaches to both pairs)
3. `odu_high -c configs/odu_high_xfapi.yaml`   (attaches as master on pair 1)

**Roles**:

| Process  | DPDK proc-type | xSM pair 0 | xSM pair 1 |
|----------|----------------|------------|------------|
| odu_low  | primary        | slave      | -          |
| XFAPI    | secondary      | master     | slave      |
| odu_high | secondary      | -          | master     |

One shared NIC config and one DPDK file-prefix (`gnb0`) for the whole host.

## 2. Two-host split (XFAPI split mode)

XFAPI is split across two servers connected by a dedicated DPDK-Ethernet link. Each host carries one side of the FAPI split locally over xSM, and the two XFAPI instances forward FAPI traffic over the wire.

```text
            Server A (L1 host)                                Server B (L2 host)
   ┌────────────────────────────────┐                ┌────────────────────────────────┐
   │                                │                │                                │
   │   ┌──────────┐   ┌──────────┐  │   DPDK-Eth     │  ┌──────────┐   ┌──────────┐   │
   │   │ odu_low  │◄─►│ XFAPI-L1 │◄─┼────────────────┼─►│ XFAPI-L2 │◄─►│ odu_high │   │
   │   │          │   │          │  │  (wire link)   │  │          │   │          │   │
   │   │ PRIMARY  │   │ SECOND.  │  │                │  │ PRIMARY  │   │ SECOND.  │   │
   │   │ SLAVE    │   │ MASTER + │  │                │  │ SLAVE +  │   │ MASTER   │   │
   │   │          │   │ Eth-MSTR │  │                │  │ Eth-MSTR │   │          │   │
   │   └──────────┘   └──────────┘  │                │  └──────────┘   └──────────┘   │
   │                                │                │                                │
   │   xsm_bridge (1 pair)          │                │   xsm_bridge (1 pair)          │
   │   file-prefix = gnb0           │                │   file-prefix = gnb0_l2        │
   └────────────────────────────────┘                └────────────────────────────────┘
```

Each host has its own `xsm_bridge` memzone with a single slot pair. The DPDK-Ethernet link between the two XFAPI instances carries serialized FAPI messages transparently to `odu_low` and `odu_high`.

**Startup order**:

1. On L1 host: `odu_low -c configs/odu_low_xfapi.yaml`             (creates the L1-side memzone)
2. On L1 host: `xfapi_main --cfgfile conf/ocudu_ocudu_split_l1.yaml`  (attaches to L1 memzone, opens NIC)
3. On L2 host: `xfapi_main --cfgfile conf/ocudu_ocudu_split_l2.yaml`  (creates L2-side memzone, opens NIC)
4. On L2 host: `odu_high -c configs/odu_high_xfapi.yaml`           (attaches to L2 memzone)

**Roles**:

| Host | Process  | DPDK proc-type | file-prefix | xSM role on local memzone | DPDK-Eth role |
|------|----------|----------------|-------------|---------------------------|---------------|
| A    | odu_low  | primary        | gnb0        | slave (pair 0)            | -             |
| A    | XFAPI-L1 | secondary      | gnb0        | master (pair 0)           | master        |
| B    | XFAPI-L2 | primary        | gnb0_l2     | slave (pair 0)            | master        |
| B    | odu_high | secondary      | gnb0_l2     | master (pair 0)           | -             |

The L1 host must have the XFAPI link NIC bound to `vfio-pci` and listed in `odu_low_xfapi.yaml`'s `hal.eal_args` (`-a <NIC_PCI_BDF>`) so XFAPI-L1 (DPDK secondary) inherits visibility of it.

## 3. OAI L1 over the bridge (disaggregated, two-host)

The L1 endpoint is an **OAI L1** rather than OCUDU's `odu_low`, deployed disaggregated across two hosts. The wire between the hosts is **nFAPI** (not xSM): OAI L1 is an nFAPI **PNF**, and xFAPI (running in **OAI_OCUDU mode**) is the nFAPI **VNF**. xSM shared memory is used only locally on Host B, between xFAPI and `odu_high`.

- **Host A — OAI L1 alone.** OAI `nr-softmodem` (openair1 PHY) drives the Liteon RU over fronthaul (PTP/GNSS synced) and exposes the L2-L1 interface as an nFAPI PNF (`NFAPI_MODE=PNF`). open-nFAPI split transport: **P5 over SCTP** (client, connects to xFAPI), **P7 over UDP** (bidirectional). Messages use big-endian TLV with ~1500 B segmentation. Runs on the Linux kernel net stack (SCTP/UDP).
- **Host B — xFAPI + OCUDU DU-High co-located.** xFAPI is the nFAPI VNF: a P5 SCTP listener that handshakes the PNF to RUNNING, and a P7 UDP listener/segmenter. In OAI_OCUDU mode it translates nFAPI ↔ FAPI and bridges onto a local `xsm_bridge` memzone (DPDK secondary, `file-prefix = gnb0_l2`), where OCUDU DU-High (`odu_high`) attaches as the xSM master on pair 1.

```text
        Host A: OAI L1                              Host B: xFAPI + OCUDU DU-High
   ┌─────────────────────────┐                ┌──────────────────────────────────────────────┐
   │  ┌───────────────────┐  │                │   ┌───────────┐  xSM pair 1   ┌─────────────┐ │
   │  │      OAI L1       │  │     nFAPI      │   │   xFAPI   │ ◄───────────► │   OCUDU     │ │
   │  │   nr-softmodem    │  │  P5 SCTP +     │   │ OAI_OCUDU │  "xsm_bridge" │   DU-High   │ │
   │  │   openair1 PHY    │◄─┼──P7 UDP────────┼──►│   mode    │   m2s / s2m   │  (odu_high) │ │
   │  │   nFAPI PNF       │  │ (kernel stack, │   │           │   zero-copy   │             │ │
   │  │   + Liteon RU     │  │  big-endian    │   │ nFAPI VNF │  (PA-ref'd)   │ xSM MASTER  │ │
   │  └───────────────────┘  │     TLV)       │   └───────────┘               │  pair 1     │ │
   │   PTP GM / GNSS synced  │                │   DPDK secondary              │ DPDK second.│ │
   └─────────────────────────┘                │   file-prefix = gnb0_l2       └─────────────┘ │
                                              └──────────────────────────────────────────────┘
```

**nFAPI socket endpoints** (Host A OAI ↔ Host B xFAPI):

- P5: OAI SCTP client → xFAPI SCTP listener on `:50001`
- P7: UDP `50010 ↔ 50011` (bidirectional)

**Startup order**:

1. On Host B: `xfapi_main --cfgfile conf/oai_ocudu_config.yaml` (creates the `xsm_bridge` memzone, opens the nFAPI P5/P7 listeners)
2. On Host A: start OAI L1 as PNF — `NFAPI_MODE=PNF nr-softmodem -O <oai-gnb.conf>` (P5 SCTP connects to xFAPI, handshake → RUNNING)
3. On Host B: `odu_high -c configs/ocudu_xfapi_oai.yaml` (attaches as xSM master on pair 1)

**Roles**:

| Host | Process  | L2-L1 wire | nFAPI role | DPDK proc-type | xSM role |
|------|----------|------------|------------|----------------|----------|
| A    | OAI L1   | nFAPI (P5 SCTP / P7 UDP) | PNF | -              | -                  |
| B    | xFAPI    | nFAPI ↔ xSM (OAI_OCUDU)   | VNF | secondary      | master/slave (pair 1, gnb0_l2) |
| B    | odu_high | xSM                       | -   | secondary      | master (pair 1)    |

`odu_high` uses [`../configs/ocudu_xfapi_oai.yaml`](../configs/ocudu_xfapi_oai.yaml) (OAI-tuned `cell_cfg`; same `fapi_split_l2` transport block as `odu_high_xfapi.yaml`).

## Choosing a topology

| Topology       | When to use                                                                 |
|----------------|-----------------------------------------------------------------------------|
| Single-host    | Functional / lab testing on one machine. Lowest latency, no NIC required.   |
| Two-host split | Production deployments where L1 and L2 must run on separate hardware.       |
| OAI L1 (disaggregated) | Pairing OCUDU DU-High with an OAI L1 over nFAPI; OAI L1 on its own host, xFAPI + `odu_high` co-located. |

Topologies 1 and 2 use the same `configs/odu_low_xfapi.yaml` and `configs/odu_high_xfapi.yaml`; only XFAPI's config file, and the `xsm_pair_index` / `dpdk_proc_type` / `xsm_file_prefix` knobs on the L2 side, select between them. Topology 3 replaces `odu_low` with an OAI L1 (nFAPI PNF) and uses `configs/ocudu_xfapi_oai.yaml` on the L2 side.
