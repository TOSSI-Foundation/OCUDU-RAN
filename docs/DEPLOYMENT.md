<!--
SPDX-License-Identifier: BSD-3-Clause-Open-MPI
-->

# OCUDU Deployment Guide  ACC100 Hardware-Accelerated DU

## Contents

1. [Prerequisites](#1-prerequisites)
2. [Host preparation](#2-host-preparation)
3. [DPDK and pf_bb_config installation](#3-dpdk-and-pf_bb_config-installation)
4. [ACC100 SR-IOV and VFIO binding](#4-acc100-sr-iov-and-vfio-binding)
5. [Building the gNB with HWACC enabled](#5-building-the-gnb-with-hwacc-enabled)
6. [DU YAML configuration](#6-du-yaml-configuration)
7. [Startup and verification](#7-startup-and-verification)
8. [Metrics and observability](#8-metrics-and-observability)
9. [Multi-VF and multi-card scaling](#9-multi-vf-and-multi-card-scaling)
10. [Troubleshooting](#10-troubleshooting)

---

## 1. Prerequisites

### 1.1 Hardware

| Component | Requirement |
|---|---|
| **Accelerator** | Intel **ACC100** PCIe card, SR-IOV capable, at least one VF exposed to the host |
| **CPU** | x86-64 with AVX2 (AVX-512 strongly recommended for the software-fallback path) |
| **Memory** | ≥ 2 GB of 2 MiB hugepages reserved at boot |
| **PCIe topology** | ACC100 slot on the **same NUMA node** as the upper-PHY worker cores |
| **NIC (for split-7.2)** | DPDK-compatible NIC with PTP  Mellanox ConnectX or Intel E810 typical |
| **Radio** | O-RU compliant with O-RAN 7.2 fronthaul, or USRP for split-8 testing |

NUMA placement is not optional. Cross-socket PCIe traffic to the ACC100 destroys the latency benefit you bought it for. Verify with `lstopo` before deployment.

### 1.2 Software

| Component | Minimum | Notes |
|---|---|---|
| **OS** | Ubuntu Server 22.04 LTS | Other distros work; this is what the team tests on |
| **Kernel** | 5.15+ | IOMMU support required; PREEMPT_RT recommended for production |
| **DPDK** | **22.11** | Tested up to 25.11; the `baseband_acc` PMD must be present |
| **pf_bb_config** | **24.03+** | Intel's PF configurator daemon; runs continuously |
| **CMake** | 3.20+ | Build system |
| **GCC / Clang** | GCC 11+ or Clang 14+ | C++17 |

### 1.3 Build flags (summary)

| Flag | Required | Purpose |
|---|---|---|
| `ENABLE_DPDK=True` | Yes | Compile DPDK integration code |
| `ENABLE_PDSCH_HWACC=True` | Yes | Compile the PDSCH (DL) LDPC offload path |
| `ENABLE_PUSCH_HWACC=True` | Yes | Compile the PUSCH (UL) LDPC offload path |
| `DU_SPLIT_TYPE=SPLIT_7_2` | For O-RAN FH | Use `SPLIT_8` for SDR/USRP testing |
| `ASSERT_LEVEL=MINIMAL` | Recommended | Reduces overhead in production builds |

---

## 2. Host preparation

### 2.1 Kernel boot parameters

Edit `/etc/default/grub`, add to `GRUB_CMDLINE_LINUX_DEFAULT`:

```
intel_iommu=on iommu=pt hugepagesz=2M hugepages=1024
```

For low-latency production, add core isolation:

```
isolcpus=2-17 nohz_full=2-17 rcu_nocbs=2-17
```

Apply and reboot:

```bash
sudo update-grub
sudo reboot
```

After reboot, verify:

```bash
cat /proc/cmdline                                  # parameters present
cat /proc/meminfo | grep HugePages_Total           # = 1024 (or your value)
dmesg | grep -i 'IOMMU enabled'                    # IOMMU active
```

### 2.2 Hugepages (runtime fallback)

If the boot-time allocation isn't honoured (e.g. memory fragmentation), top up at runtime:

```bash
echo 1024 | sudo tee /sys/kernel/mm/hugepages/hugepages-2048kB/nr_hugepages
sudo mkdir -p /mnt/huge
sudo mount -t hugetlbfs nodev /mnt/huge
```

### 2.3 VFIO modules

```bash
sudo modprobe vfio-pci
# Only needed if vfio-pci is kernel-builtin and SR-IOV not auto-enabled:
echo 1 | sudo tee /sys/module/vfio_pci/parameters/enable_sriov
```

Confirm:

```bash
lsmod | grep vfio
```

---

## 3. DPDK and pf_bb_config installation

### 3.1 DPDK build

Tested with DPDK 22.11 LTS and 25.11. Example for 25.11:

```bash
cd /opt
sudo wget https://fast.dpdk.org/rel/dpdk-25.11.tar.xz
sudo tar xf dpdk-25.11.tar.xz
cd dpdk-25.11

sudo apt-get install -y meson ninja-build python3-pyelftools libnuma-dev pkg-config
sudo meson setup -Dexamples=all build
sudo ninja -C build
sudo ninja -C build install
sudo ldconfig
```

Verify the ACC100 PMD is present:

```bash
dpdk-devbind.py --status-dev baseband
# Expect to see 'baseband_acc' or 'intel_acc100_vf' once VFs are bound (§4)
```

### 3.2 pf_bb_config

```bash
cd /opt
sudo git clone https://github.com/intel/pf-bb-config.git
cd pf-bb-config
sudo ./build.sh
```

Binary lands at `/opt/pf-bb-config/pf_bb_config`. Configuration files for ACC100 5G LDPC live under `/opt/pf-bb-config/acc100/`.

---

## 4. ACC100 SR-IOV and VFIO binding

### 4.1 Identify the PF

```bash
lspci | grep -i acc100
# Example: 65:00.0 Processing accelerators: Intel Corporation Device 0d5c
```

The `65:00.0` is your **PF BDF**. Export it for convenience:

```bash
export ACC100_PF_BDF=0000:65:00.0
```

### 4.2 Create one VF and bind to vfio-pci

```bash
echo 1 | sudo tee /sys/bus/pci/devices/${ACC100_PF_BDF}/sriov_numvfs

# The VF appears as a new BDF; identify it:
lspci | grep -i acc100
# Example new line: 65:00.1 ... Device 0d5d   <-- this is the VF

export ACC100_VF_BDF=0000:65:00.1

sudo dpdk-devbind.py --bind=vfio-pci ${ACC100_VF_BDF}
```

Verify:

```bash
dpdk-devbind.py --status-dev baseband
# Drv=vfio-pci on the VF line confirms binding
```

### 4.3 Start pf_bb_config

`pf_bb_config` must run continuously while the DU is running  it holds the PF open with a VF token (UUID) that the DU's DPDK process needs in order to attach to the VF. **If `pf_bb_config` exits, in-flight DU operations to the accelerator will fail.**

```bash
# Generate a UUID once and keep it; it goes into the DU YAML too.
export PF_BB_UUID=$(uuidgen)
echo "PF_BB_UUID=${PF_BB_UUID}"   # SAVE THIS

sudo /opt/pf-bb-config/pf_bb_config ACC100 \
     -c /opt/pf-bb-config/acc100/acc100_config_vf_5g.cfg \
     -v ${PF_BB_UUID} &
```

For production, run `pf_bb_config` under systemd with a stable UUID stored in `/etc/ocudu/pf_bb.uuid`. Restarting `pf_bb_config` requires updating the DU YAML and restarting the gNB.

### 4.4 Sanity check with dpdk-test-bbdev

```bash
sudo dpdk-test-bbdev -c 0x3 -a ${ACC100_VF_BDF} \
     --vfio-vf-token=${PF_BB_UUID}
```

The VF should enumerate as `intel_acc100_vf`. If this command fails, do not proceed  fix the binding/token first.

---

## 5. Building the gNB with HWACC enabled

### 5.1 gNB with 7.2 fronthaul + ACC100

```bash
cd ~/ocudu        # or wherever you cloned this fork
mkdir -p build_hwacc && cd build_hwacc

sudo cmake -DDU_SPLIT_TYPE=SPLIT_7_2 \
           -DENABLE_DPDK=True \
           -DENABLE_PDSCH_HWACC=True \
           -DENABLE_PUSCH_HWACC=True \
           -DASSERT_LEVEL=MINIMAL \
           ../

sudo make -j$(nproc)
```

Resulting binary: `build_hwacc/apps/gnb/gnb`.

### 5.2 Optional: PHY benchmarks

For standalone offload measurements without an RU/UE in the loop:

```bash
cd build_hwacc
sudo make -j$(nproc) pdsch_processor_benchmark pusch_processor_benchmark
```

Produces `pdsch_processor_benchmark` and `pusch_processor_benchmark` binaries  the same harnesses used for the figures in [`ACC100_INTEGRATION.md`](ACC100_INTEGRATION.md) §7.

### 5.3 Build flag reference

The three flags `ENABLE_DPDK`, `ENABLE_PDSCH_HWACC`, `ENABLE_PUSCH_HWACC` are independent. You can build a binary with PDSCH offload only (`ENABLE_PUSCH_HWACC=False`), for example, if you want to A/B only one direction. The DU YAML decides whether each path is *used* at runtime (§6); the build flags decide whether the path is *compiled in*.

---

## 6. DU YAML configuration

The accelerator is enabled entirely from the DU YAML. There are no environment-variable or CLI knobs.

### 6.1 Minimum HAL block

```yaml
hal:
  eal_args: "--lcores (0-1)@(0-17) --file-prefix=ocudu_gnb --no-telemetry
             -a <OFH_NIC_VF_BDF>
             -a <ACC100_VF_BDF>
             --vfio-vf-token=<PF_BB_CONFIG_UUID>
             --iova-mode=pa"
  bbdev_hwacc:
    hwacc_type: "acc100"
    id: 0
    pdsch_enc:
      nof_hwacc: 4
      cb_mode: true
      dedicated_queue: true
    pusch_dec:
      nof_hwacc: 4
      force_local_harq: false
      dedicated_queue: true
```

Substitutions:

- `<OFH_NIC_VF_BDF>`  the BDF of the fronthaul NIC VF (e.g. `0000:18:00.1`).
- `<ACC100_VF_BDF>`  the BDF you bound in §4.2.
- `<PF_BB_CONFIG_UUID>`  the UUID you passed to `pf_bb_config -v` in §4.3.

### 6.2 Knobs that matter

| Field | Default | Guidance |
|---|---|---|
| `nof_hwacc` (per direction) | 4 | Single-cell deployments: **2–4**. Multi-cell or heavy-load: **8–16**. Setting higher than the upper-PHY's concurrency limits is wasted memory; the DU will warn and clamp to the upper-PHY value (see §7). |
| `cb_mode` (PDSCH) | `true` | Code-block mode is the only supported PDSCH encode mode at present. Transport-block mode is a future optimisation; single-CB TBs are still handled correctly. |
| `force_local_harq` (PUSCH) | `false` | Keep `false` to use on-chip HARQ buffer (the design point of ACC100). Only set `true` for debugging soft-data correctness against software. |
| `dedicated_queue` | `true` | Each accelerated function gets its own BBDEV queue. Set to `false` only if you are queue-starved (rare). |
| `--iova-mode=pa` |  | Recommended. The `va` mode interacts poorly with some NIC drivers in DPDK 25.11. |

### 6.3 Example: full split-7.2 gNB skeleton

```yaml
gnb_id: 1
ru_ofh:
  ru_bandwidth_mhz: 100
  t1a_max_cp_dl: 470
  # ... fronthaul timing parameters per O-RU spec ...
cell_cfg:
  dl_arfcn: 632628
  band: 78
  channel_bandwidth_MHz: 100
  common_scs: 30
  pci: 1
  plmn: "00101"
  tac: 7
hal:
  eal_args: "--lcores (0-1)@(0-17) --file-prefix=ocudu_gnb --no-telemetry
             -a 0000:18:00.1 -a 0000:65:00.1
             --vfio-vf-token=12345678-1234-1234-1234-1234567890ab
             --iova-mode=pa"
  bbdev_hwacc:
    hwacc_type: "acc100"
    id: 0
    pdsch_enc:
      nof_hwacc: 4
      cb_mode: true
      dedicated_queue: true
    pusch_dec:
      nof_hwacc: 4
      force_local_harq: false
      dedicated_queue: true
metrics:
  enable_log: true
  enable_verbose: true
  layers:
    enable_du_low: true
  periodicity:
    du_report_period: 1000
log:
  filename: /tmp/ocudu_gnb.log
  all_level: info
  hal_level: info
```

The `metrics` block is required if you want the side-by-side LDPC comparison numbers in the log (§8). The `hal_level: info` ensures the BBDEV startup banner is logged at startup (§7).

---

## 7. Startup and verification

### 7.1 Run

```bash
cd build_hwacc
sudo ./apps/gnb/gnb -c /etc/ocudu/gnb_acc100.yaml
```

### 7.2 What to expect in the log

On successful HWACC bring-up:

```
[HWACC] [I] [bbdev] dev=0 driver=intel_acc100_vf ...
[HWACC] [I] [bbdev] dev=0 started: ldpc_enc_q=N ldpc_dec_q=N ...
```

If `nof_hwacc` exceeds the upper-PHY concurrency, the DU prints (and clamps):

```
Warning: the configured maximum PDSCH concurrency ... is overridden by the
         number of PDSCH encoder hardware accelerated functions (N)
Warning: the configured maximum PUSCH and SRS concurrency ... is overridden
         by the number of PUSCH decoder hardware accelerated functions (N)
```

These warnings are informational  the DU is *using* the accelerator. They mean you over-provisioned `nof_hwacc`; lower it next restart to free memory.

### 7.3 Confirming the accelerator is actually on the data path

Two cross-checks beyond the startup banner:

1. **Metrics** (§8)  the `LDPC Decoder` block populates with HW-path latencies (typically ≤ 50 µs avg) instead of software latencies (typically ~50–110 µs avg).
2. **CPU usage**  `upper_phy_ul` drops by ~30–40 % under sustained traffic; `ldpc_rm` and `ldpc_rdm` go to ~0 %. If they don't, the DU is silently falling back to software (check the log for fallback warnings, and re-verify §4).

---

## 8. Metrics and observability

### 8.1 Enabling DU-low metrics

In the YAML:

```yaml
metrics:
  enable_log: true
  enable_verbose: true
  layers:
    enable_du_low: true
  periodicity:
    du_report_period: 1000   # ms
```

### 8.2 What you get

Every `du_report_period` milliseconds, the log emits a block similar to:

```
LDPC Encoder:  avg_cb_size=… bits, avg_latency=… us, encode_rate=… Mbps
LDPC Decoder:  avg_cb_size=… bits, avg_latency=… us, avg_nof_iter=…, decode_rate=… Mbps
PDSCH Processor: avg_latency=… us, max_latency=… us, proc_rate=… Mbps
PUSCH Processor: avg_data_latency=… us, proc_rate=… Mbps
CPU usage:     upper_phy_dl=…%, ldpc_enc=…%, …
               upper_phy_ul=…%, ldpc_dec=…%, …
```

The same field set populates whether LDPC is running on the accelerator or in software, which is the key A/B affordance: you can directly diff two log files from the same workload.

### 8.3 Caveat on per-CB encoder timings

On the HW path, the per-CB `ldpc_encoder_*` fields reflect **batch wall-clock time** rather than serialised per-CB compute time, because ops are submitted to the accelerator in bursts. For apples-to-apples DL comparison, use the **`PDSCH Processor`** rows and **`upper_phy_dl`** percentage  those are measured once per TB and are directly comparable to the software path.

This caveat does *not* apply to the decoder path; `LDPC Decoder` per-CB fields are accurate on both software and HW.

---

## 9. Multi-VF and multi-card scaling

A single ACC100 saturates at a finite aggregate LDPC rate  single-card throughput is enough for typical single-cell or small multi-cell deployments, but heavy multi-cell loads or aggressive MCS profiles can hit the ceiling.

Scaling out is straightforward: **allowlist additional ACC100 VFs** in the EAL args, then increase `nof_hwacc` accordingly. The HAL's shared mbuf and op pools size automatically with the total queue count across allowlisted accelerators; no other configuration changes are required.

```yaml
hal:
  eal_args: "--lcores (0-1)@(0-17) --file-prefix=ocudu_gnb --no-telemetry
             -a 0000:18:00.1
             -a 0000:65:00.1
             -a 0000:65:00.2          # second VF on the same card
             -a 0000:b1:00.1          # VF on a second card on a second NUMA node
             --vfio-vf-token=12345678-1234-1234-1234-1234567890ab
             --iova-mode=pa"
  bbdev_hwacc:
    hwacc_type: "acc100"
    id: 0
    pdsch_enc:
      nof_hwacc: 12
      cb_mode: true
      dedicated_queue: true
    pusch_dec:
      nof_hwacc: 12
      force_local_harq: false
      dedicated_queue: true
```

Two operational notes:

- All VFs must share the same `pf_bb_config` UUID at runtime, or be configured by separate `pf_bb_config` instances using distinct UUIDs (the EAL accepts only one `--vfio-vf-token`, so the simpler path is one UUID).
- Cards on different NUMA nodes still work, but for latency-sensitive deployments you usually want one card per NUMA node and one upper-PHY worker pool per node.

---

## 10. Troubleshooting

### 10.1 DU exits at startup with "no bbdev devices found"

- VF not bound to `vfio-pci`  re-run §4.2; check `dpdk-devbind.py --status-dev baseband`.
- VF BDF in the YAML is wrong  `lspci | grep -i acc100` to confirm.
- IOMMU disabled  `dmesg | grep -i iommu`; re-check kernel cmdline (§2.1).

### 10.2 DU logs "vfio-vf-token mismatch"

- The UUID in `--vfio-vf-token=…` doesn't match the UUID `pf_bb_config` was started with. They must be byte-identical.
- If `pf_bb_config` was restarted with a fresh UUID, update the YAML and restart the DU.

### 10.3 DU starts but metrics show software-path latencies

- Look for HAL-level fallback warnings in the log (`hwacc fallback`, `dropping to software`, etc.).
- The most common cause is `pf_bb_config` having died  `ps -ef | grep pf_bb_config` should show it running. If not, restart it; the DU does **not** auto-recover, restart that too.
- Less common: the operation exceeded the ACC100 per-op E-limit (a pathological MCS/PRB combination). Software fallback handled it correctly, but it indicates a scheduler-side anomaly  open an issue with the offending TBS/MCS pair.

### 10.4 Hugepage allocation fails

- Memory is too fragmented for 2 MiB pages  reboot, or use 1 GiB pages (`hugepagesz=1G hugepages=2`).
- Some other process already grabbed the hugepages  `cat /proc/meminfo | grep Huge` to inspect.

### 10.5 NUMA placement is wrong

- `lstopo --logical | less` to confirm which NUMA node the ACC100 PCI slot is on, and which cores are on the same node.
- Adjust the EAL `--lcores` mapping to use cores on the same node as the accelerator.

### 10.6 Low-SINR uplink is worse than software

- Known limitation: the upper-PHY currently passes already-quantised int8 LLRs to the HAL; on 2+ layer MIMO at low SINR this can cost a fraction of a dB compared to the software path's wider intermediate precision.
- If this matters for your deployment, run with `force_local_harq: true` for diagnostic logging, then file an issue. A future enhancement adds dynamic LLR scaling.

### 10.7 General log triage

| Symptom | Most likely cause | First check |
|---|---|---|
| Crash at EAL init | Hugepages not allocated | `/proc/meminfo` |
| Crash binding NIC | NIC not bound to vfio-pci, or wrong driver | `dpdk-devbind.py --status` |
| HWACC banner missing | Build didn't include HWACC | rebuild with the three `ENABLE_*` flags |
| Fallback warnings during traffic | pf_bb_config died, or queue exhaustion | check pf_bb_config; raise `nof_hwacc` |
| Throughput plateau below expected | Single-card saturation | add VFs (§9) |
