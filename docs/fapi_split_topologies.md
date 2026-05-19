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

## Choosing a topology

| Topology       | When to use                                                                 |
|----------------|-----------------------------------------------------------------------------|
| Single-host    | Functional / lab testing on one machine. Lowest latency, no NIC required.   |
| Two-host split | Production deployments where L1 and L2 must run on separate hardware.       |

Both topologies use the same `configs/odu_low_xfapi.yaml` and `configs/odu_high_xfapi.yaml`. Only XFAPI's config file, and the `xsm_pair_index` / `dpdk_proc_type` / `xsm_file_prefix` knobs on the L2 side, select between them.
