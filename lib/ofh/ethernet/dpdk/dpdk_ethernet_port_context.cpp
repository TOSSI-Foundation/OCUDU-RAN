// SPDX-FileCopyrightText: Copyright (C) 2021-2026 Software Radio Systems Limited
// SPDX-License-Identifier: BSD-3-Clause-Open-MPI

#include "ocudu/ofh/ethernet/dpdk/dpdk_ethernet_port_context.h"
#include <algorithm>
#include <array>
#include <charconv>
#include <chrono>
#include <optional>
#include <rte_byteorder.h>
#include <rte_ethdev.h>
#include <rte_flow.h>
#include <rte_mbuf.h>

#ifdef ENABLE_GPU_FRONTHAUL
#include "ocudu/hal/cuda/gpu_dpdk_mempool.h"
#include "ocudu/hal/cuda/inline_prach_pipeline.h"
#include "ocudu/hal/cuda/inline_srs_pipeline.h"
#include "ocudu/support/srs_schedule_tap.h"
#include <cstdio>
#include <cstdlib>
#include <cuda_runtime.h>
#endif

using namespace ocudu;
using namespace ether;

#ifdef ENABLE_GPU_FRONTHAUL
static bool prach_bench_logs()
{
  static const bool on = std::getenv("OCUDU_PRACH_BENCH") != nullptr;
  return on;
}
#endif

/// DPDK configuration settings.
static constexpr unsigned MBUF_CACHE_SIZE = 256;
static constexpr unsigned RX_RING_SIZE    = 1024;
static constexpr unsigned TX_RING_SIZE    = 1024;
static constexpr unsigned NUM_MBUFS       = 13824;

static constexpr unsigned GPU_HEADER_PEEK = 48;

#ifdef ENABLE_GPU_FRONTHAUL
static ::rte_flow* install_ethertype_only_rule(uint16_t port_id, uint16_t queue_id)
{
  ::rte_flow_attr attr{};
  attr.ingress  = 1;
  attr.priority = 1;

  ::rte_flow_action_queue qa{};
  qa.index = queue_id;
  ::rte_flow_action actions[] = {
      {.type = RTE_FLOW_ACTION_TYPE_QUEUE, .conf = &qa},
      {.type = RTE_FLOW_ACTION_TYPE_END,   .conf = nullptr},
  };

  ::rte_flow_item_eth  vlan_eth_spec{};
  ::rte_flow_item_eth  vlan_eth_mask{};
  vlan_eth_spec.hdr.ether_type = rte_cpu_to_be_16(0x8100);
  vlan_eth_mask.hdr.ether_type = 0xFFFF;

  ::rte_flow_item_vlan vlan_spec{};
  ::rte_flow_item_vlan vlan_mask{};
  vlan_spec.hdr.eth_proto = rte_cpu_to_be_16(0xAEFE);
  vlan_mask.hdr.eth_proto = 0xFFFF;

  ::rte_flow_item vlan_pattern[] = {
      {.type = RTE_FLOW_ITEM_TYPE_ETH,  .spec = &vlan_eth_spec, .last = nullptr, .mask = &vlan_eth_mask},
      {.type = RTE_FLOW_ITEM_TYPE_VLAN, .spec = &vlan_spec,     .last = nullptr, .mask = &vlan_mask},
      {.type = RTE_FLOW_ITEM_TYPE_END,  .spec = nullptr,        .last = nullptr, .mask = nullptr},
  };
  ::rte_flow_error err{};
  if (::rte_flow_validate(port_id, &attr, vlan_pattern, actions, &err) == 0) {
    ::rte_flow* h = ::rte_flow_create(port_id, &attr, vlan_pattern, actions, &err);
    if (h != nullptr) {
      fmt::print("DPDK GPU - installed ETH(0x8100)+VLAN(inner=0xAEFE) -> queue {}\n", queue_id);
      return h;
    }
    fmt::print("DPDK GPU - VLAN+eCPRI rule create failed: type={} msg={}\n",
               static_cast<int>(err.type),
               err.message ? err.message : "(none)");
  } else {
    fmt::print("DPDK GPU - VLAN+eCPRI rule validate failed: type={} msg={} — trying untagged\n",
               static_cast<int>(err.type),
               err.message ? err.message : "(none)");
  }

  ::rte_flow_item_eth eth_spec{};
  ::rte_flow_item_eth eth_mask{};
  eth_spec.hdr.ether_type = rte_cpu_to_be_16(0xAEFE);
  eth_mask.hdr.ether_type = 0xFFFF;
  ::rte_flow_item bare_pattern[] = {
      {.type = RTE_FLOW_ITEM_TYPE_ETH, .spec = &eth_spec, .last = nullptr, .mask = &eth_mask},
      {.type = RTE_FLOW_ITEM_TYPE_END, .spec = nullptr,   .last = nullptr, .mask = nullptr},
  };
  if (::rte_flow_validate(port_id, &attr, bare_pattern, actions, &err) != 0) {
    fmt::print("DPDK GPU - untagged ethertype rule validate failed: type={} msg={}\n",
               static_cast<int>(err.type),
               err.message ? err.message : "(none)");
    return nullptr;
  }
  ::rte_flow* h = ::rte_flow_create(port_id, &attr, bare_pattern, actions, &err);
  if (h != nullptr) {
    fmt::print("DPDK GPU - installed ETH(0xAEFE) -> queue {} (untagged fallback)\n", queue_id);
  } else {
    fmt::print("DPDK GPU - untagged ethertype rule create failed: type={} msg={}\n",
               static_cast<int>(err.type),
               err.message ? err.message : "(none)");
  }
  return h;
}

static ::rte_flow* try_install_per_eaxc_rule(uint16_t port_id, uint16_t eaxc, uint16_t queue_id)
{
  ::rte_flow_item_eth eth_spec{};
  ::rte_flow_item_eth eth_mask{};
  eth_spec.hdr.ether_type = rte_cpu_to_be_16(0xAEFE);
  eth_mask.hdr.ether_type = 0xFFFF;

  ::rte_flow_item_ecpri ecpri_type_spec{};
  ::rte_flow_item_ecpri ecpri_type_mask{};
  ecpri_type_spec.hdr.common.revision = 0x1;
  ecpri_type_spec.hdr.common.type     = 0;
  ecpri_type_mask.hdr.common.revision = 0xf;
  ecpri_type_mask.hdr.common.res      = 0x7;
  ecpri_type_mask.hdr.common.c        = 0x1;
  ecpri_type_mask.hdr.common.type     = 0xff;

  ::rte_flow_item_ecpri ecpri_pcid_spec{};
  ::rte_flow_item_ecpri ecpri_pcid_mask{};
  ecpri_pcid_spec.hdr.type0.pc_id = rte_cpu_to_be_16(eaxc);
  ecpri_pcid_mask.hdr.type0.pc_id = 0xffff;

  ::rte_flow_item pattern[] = {
      {.type = RTE_FLOW_ITEM_TYPE_ETH,   .spec = &eth_spec,         .last = nullptr, .mask = &eth_mask},
      {.type = RTE_FLOW_ITEM_TYPE_ECPRI, .spec = &ecpri_type_spec,  .last = nullptr, .mask = &ecpri_type_mask},
      {.type = RTE_FLOW_ITEM_TYPE_ECPRI, .spec = &ecpri_pcid_spec,  .last = nullptr, .mask = &ecpri_pcid_mask},
      {.type = RTE_FLOW_ITEM_TYPE_END,   .spec = nullptr,           .last = nullptr, .mask = nullptr},
  };
  ::rte_flow_action_queue qa{};
  qa.index = queue_id;
  ::rte_flow_action actions[] = {
      {.type = RTE_FLOW_ACTION_TYPE_QUEUE, .conf = &qa},
      {.type = RTE_FLOW_ACTION_TYPE_END,   .conf = nullptr},
  };
  ::rte_flow_attr attr{};
  attr.ingress  = 1;
  attr.priority = 0;

  ::rte_flow_error err{};
  if (::rte_flow_validate(port_id, &attr, pattern, actions, &err) != 0) {
    return nullptr;
  }
  ::rte_flow* h = ::rte_flow_create(port_id, &attr, pattern, actions, &err);
  return h;
}

static std::vector<::rte_flow*> install_gpu_steering_rules(uint16_t                     port_id,
                                                            const std::vector<uint16_t>& prach_eaxcs,
                                                            uint16_t                     queue_id,
                                                            bool&                        used_per_eaxc)
{
  std::vector<::rte_flow*> rules;
  used_per_eaxc = false;

  if (!prach_eaxcs.empty()) {
    bool all_ok = true;
    for (uint16_t e : prach_eaxcs) {
      ::rte_flow* h = try_install_per_eaxc_rule(port_id, e, queue_id);
      if (h == nullptr) {
        all_ok = false;
        break;
      }
      rules.push_back(h);
    }
    if (all_ok) {
      used_per_eaxc = true;
      return rules;
    }
    for (::rte_flow* h : rules) {
      ::rte_flow_error err{};
      ::rte_flow_destroy(port_id, h, &err);
    }
    rules.clear();
  }

  if (::rte_flow* h = install_ethertype_only_rule(port_id, queue_id); h != nullptr) {
    rules.push_back(h);
  } else {
    return rules;
  }

  if (::rte_flow* h_host = install_ethertype_only_rule(port_id,  0); h_host != nullptr) {
    rules.push_back(h_host);
    fmt::print("DPDK GPU - installed duplicate host-queue rule (queue 0) — non-PRACH eCPRI now reaches the CPU OFH receiver natively\n");
  } else {
    fmt::print("DPDK GPU - WARN: host-queue duplicate rule REJECTED by mlx5 — non-PRACH eCPRI (PUSCH/PUCCH/SRS) will be silently dropped by the GPU listener. `prach_rx_to_gpu: true` is dev-only in this configuration.\n");
  }
  return rules;
}

static void gpu_listener_loop(uint16_t                     port_id,
                              uint16_t                     queue_id,
                              std::vector<uint16_t>        prach_eaxcs,
                              ::rte_mempool*               gpu_pool,
                              std::atomic<bool>&           stop,
                              std::atomic<uint64_t>&       prach_count,
                              std::atomic<uint64_t>&       srs_count,
                              std::atomic<uint64_t>&       other_count,
                              std::shared_ptr<hal::cuda::inline_prach_pipeline> inline_pipeline,
                              unsigned                     iq_payload_offset_bytes,
                              bool                         srs_classify_enabled,
                              std::vector<uint16_t>        ul_eaxcs,
                              std::shared_ptr<hal::cuda::inline_srs_pipeline> srs_pipeline)
{
  std::array<::rte_mbuf*, 32> mbufs;
  std::array<uint8_t, GPU_HEADER_PEEK> host_hdr{};

  auto     last_log    = std::chrono::steady_clock::now();
  uint64_t peek_fail   = 0;
  uint64_t cuda_fail   = 0;
  uint64_t total_bursts = 0;
  uint64_t total_pkts   = 0;
  uint16_t last_pc_id  = 0xFFFF;
  uint8_t  last_msgtyp = 0xFF;

  fmt::print(stderr,
             "[gpu_listener] thread started: port={} queue={} prach_eaxcs_count={}\n",
             port_id,
             queue_id,
             prach_eaxcs.size());

  bool     force_srs_enabled = false;
  unsigned force_srs_sf = 0, force_srs_slot = 0, force_srs_sym = 0;
  if (const char* fenv = std::getenv("OCUDU_SRS_GPU_FORCE")) {
    if (std::sscanf(fenv, "%u:%u:%u", &force_srs_sf, &force_srs_slot, &force_srs_sym) == 3) {
      force_srs_enabled = true;
      fmt::print(stderr,
                 "[gpu_listener] OCUDU_SRS_GPU_FORCE active — forcing SRS classification on "
                 "(sf={}, slot={}, sym={})\n",
                 force_srs_sf, force_srs_slot, force_srs_sym);
    }
  }
  bool                       force_res_enabled = false;
  uint8_t                    force_res_nrx     = 4;
  srs_resource_configuration force_res{};
  if (const char* renv = std::getenv("OCUDU_SRS_GPU_RES")) {
    unsigned csrs = 0, bsrs = 0, fpos = 0, fshift = 0, comb = 4, coff = 0, seqid = 0, nsym = 1, nrx = 4;
    if (std::sscanf(renv, "%u:%u:%u:%u:%u:%u:%u:%u:%u", &csrs, &bsrs, &fpos, &fshift, &comb, &coff, &seqid,
                    &nsym, &nrx) == 9) {
      force_res.nof_antenna_ports   = srs_resource_configuration::one_two_four_enum::one;
      force_res.nof_symbols         = static_cast<srs_nof_symbols>(nsym);
      force_res.start_symbol        = static_cast<uint8_t>(force_srs_sym);
      force_res.configuration_index = static_cast<uint8_t>(csrs);
      force_res.sequence_id         = static_cast<uint16_t>(seqid);
      force_res.bandwidth_index     = static_cast<uint8_t>(bsrs);
      force_res.comb_size           = (comb == 2) ? tx_comb_size::n2 : tx_comb_size::n4;
      force_res.comb_offset         = static_cast<uint8_t>(coff);
      force_res.cyclic_shift        = 0;
      force_res.freq_position       = static_cast<uint8_t>(fpos);
      force_res.freq_shift          = static_cast<uint16_t>(fshift);
      force_res.freq_hopping        = 0;
      force_res.hopping             = srs_group_or_sequence_hopping::neither;
      force_res_nrx                 = static_cast<uint8_t>(nrx);
      force_res_enabled             = true;
      fmt::print(stderr,
                 "[gpu_listener] OCUDU_SRS_GPU_RES active — synthetic SRS resource (csrs={} bsrs={} "
                 "freqpos={} comb={} seqid={} nsym={} nrx={})\n",
                 csrs, bsrs, fpos, comb, seqid, nsym, nrx);
    }
  }

  while (!stop.load(std::memory_order_relaxed)) {
    const unsigned nb = ::rte_eth_rx_burst(port_id, queue_id, mbufs.data(), mbufs.size());
    if (nb > 0) {
      ++total_bursts;
      total_pkts += nb;
    } else {
      std::this_thread::sleep_for(std::chrono::microseconds(5));
    }
    for (unsigned i = 0; i < nb; ++i) {
      ::rte_mbuf* m            = mbufs[i];
      void* const vram_data    = static_cast<uint8_t*>(m->buf_addr) + m->data_off;
      const unsigned peek_len  = std::min<unsigned>(m->data_len, GPU_HEADER_PEEK);
      cudaError_t err = cudaMemcpy(host_hdr.data(), vram_data, peek_len, cudaMemcpyDeviceToHost);
      if (err != cudaSuccess) {
        ++cuda_fail;
      } else if (peek_len < 22) {
        ++peek_fail;
      }
      if (err == cudaSuccess && peek_len >= 22) {
        unsigned off = 14;
        uint16_t et  = (static_cast<uint16_t>(host_hdr[12]) << 8) | host_hdr[13];
        if (et == 0x8100 || et == 0x88a8) {
          off = 18;
        }
        if (off + 6 <= peek_len) {
          const uint8_t  msg_type  = host_hdr[off + 1];
          const uint16_t pc_id     = (static_cast<uint16_t>(host_hdr[off + 4]) << 8) | host_hdr[off + 5];
          bool           is_prach  = false;
          if (msg_type == 0) {
            for (uint16_t e : prach_eaxcs) {
              if (e == pc_id) {
                is_prach = true;
                break;
              }
            }
          }
          last_pc_id  = pc_id;
          last_msgtyp = msg_type;

          bool     have_ofh       = false;
          uint32_t packed_slot_id = 0;
          unsigned symbol_idx     = 0;
          unsigned ofh_start_prb  = 0;
          unsigned ofh_num_prbu   = 0;
          if (peek_len >= 30 && off + 16 <= peek_len) {
            const unsigned ofh_hdr_off = off + 8;
            const uint8_t  sfn         = host_hdr[ofh_hdr_off + 1];
            const uint8_t  b_subslot   = host_hdr[ofh_hdr_off + 2];
            const uint8_t  b_slotsym   = host_hdr[ofh_hdr_off + 3];
            const unsigned subframe    = (b_subslot >> 4) & 0x0Fu;
            const unsigned slot_in_sf  = ((b_subslot & 0x0Fu) << 2) | ((b_slotsym >> 6) & 0x03u);
            symbol_idx                 = b_slotsym & 0x3Fu;
            packed_slot_id =
                (static_cast<uint32_t>(sfn) << 16) | (static_cast<uint32_t>(subframe) << 8) | slot_in_sf;
            ofh_start_prb = ((static_cast<unsigned>(host_hdr[off + 13]) & 0x03u) << 8) | host_hdr[off + 14];
            ofh_num_prbu  = host_hdr[off + 15];
            have_ofh = true;
          }

          if (is_prach) {
            prach_count.fetch_add(1, std::memory_order_relaxed);

            if (inline_pipeline && have_ofh) {
              unsigned eaxc_idx = 0;
              for (unsigned k = 0; k < prach_eaxcs.size(); ++k) {
                if (prach_eaxcs[k] == pc_id) {
                  eaxc_idx = k;
                  break;
                }
              }
              const uint8_t* vram_iq_ptr =
                  static_cast<const uint8_t*>(vram_data) + iq_payload_offset_bytes;
              ::rte_mbuf* hold_m   = m;
              auto         release = [hold_m]() {
                ::rte_mbuf* seg = hold_m->next;
                ::rte_pktmbuf_free_seg(hold_m);
                while (seg != nullptr) {
                  ::rte_mbuf* nxt = seg->next;
                  ::rte_pktmbuf_free_seg(seg);
                  seg = nxt;
                }
              };
              inline_pipeline->on_packet(packed_slot_id, symbol_idx, eaxc_idx, vram_iq_ptr, std::move(release));
              continue;
            }
          } else if (srs_classify_enabled && msg_type == 0 && have_ofh &&
                     (srs_schedule_tap::is_srs_symbol(packed_slot_id, static_cast<uint8_t>(symbol_idx)) ||
                      (force_srs_enabled && ((packed_slot_id >> 8) & 0xFFu) == force_srs_sf &&
                       (packed_slot_id & 0xFFu) == force_srs_slot && symbol_idx == force_srs_sym))) {
            srs_count.fetch_add(1, std::memory_order_relaxed);

            if (force_res_enabled && ((packed_slot_id >> 8) & 0xFFu) == force_srs_sf &&
                (packed_slot_id & 0xFFu) == force_srs_slot && symbol_idx == force_srs_sym) {
              srs_schedule_tap::publish(packed_slot_id, static_cast<uint8_t>(force_srs_sym),
                                        static_cast<uint8_t>(force_res.nof_symbols), force_res, force_res_nrx);
            }

            if (srs_pipeline) {
              srs_resource_configuration sched_res{};
              uint8_t                    sched_nof_rx = 0;
              const bool                 have_res =
                  srs_schedule_tap::lookup_resource(packed_slot_id, sched_res, sched_nof_rx) && sched_nof_rx > 0;
              unsigned rx_port_idx = 0;
              bool     port_found  = false;
              for (unsigned k = 0; k < ul_eaxcs.size(); ++k) {
                if (ul_eaxcs[k] == pc_id) {
                  rx_port_idx = k;
                  port_found  = true;
                  break;
                }
              }
              if (have_res && port_found) {
                const uint8_t* vram_iq_ptr =
                    static_cast<const uint8_t*>(vram_data) + iq_payload_offset_bytes;
                ::rte_mbuf* hold_m  = m;
                auto        release = [hold_m]() {
                  ::rte_mbuf* seg = hold_m->next;
                  ::rte_pktmbuf_free_seg(hold_m);
                  while (seg != nullptr) {
                    ::rte_mbuf* nxt = seg->next;
                    ::rte_pktmbuf_free_seg(seg);
                    seg = nxt;
                  }
                };
                srs_pipeline->on_packet(packed_slot_id, symbol_idx, rx_port_idx, ofh_num_prbu,
                                        ofh_start_prb, sched_res, sched_nof_rx, vram_iq_ptr,
                                        std::move(release));
                continue;
              }
            }
          } else {
            other_count.fetch_add(1, std::memory_order_relaxed);
          }
        }
      }
      ::rte_mbuf* seg = m->next;
      ::rte_pktmbuf_free_seg(m);
      while (seg != nullptr) {
        ::rte_mbuf* nxt = seg->next;
        ::rte_pktmbuf_free_seg(seg);
        seg = nxt;
      }
    }

    auto now = std::chrono::steady_clock::now();
    if (prach_bench_logs() && std::chrono::duration_cast<std::chrono::seconds>(now - last_log).count() >= 1) {
      uint64_t q0_pkts = UINT64_MAX, q1_pkts = UINT64_MAX;
      uint64_t ids[2]  = {0, 0};
      const char* names[2] = {"rx_q0_packets", "rx_q1_packets"};
      if (::rte_eth_xstats_get_id_by_name(port_id, names[0], &ids[0]) == 0 &&
          ::rte_eth_xstats_get_id_by_name(port_id, names[1], &ids[1]) == 0) {
        uint64_t vals[2] = {0, 0};
        if (::rte_eth_xstats_get_by_id(port_id, ids, vals, 2) == 2) {
          q0_pkts = vals[0];
          q1_pkts = vals[1];
        }
      }
      const unsigned mp_avail = gpu_pool != nullptr ? ::rte_mempool_avail_count(gpu_pool) : 0;
      const unsigned mp_inuse = gpu_pool != nullptr ? ::rte_mempool_in_use_count(gpu_pool) : 0;
      fmt::print(stderr,
                 "[gpu_listener] bursts={} pkts={} prach={} srs={} other={} peek_fail={} cuda_fail={} "
                 "last_msg_type=0x{:02x} last_pc_id={} nic_q0_pkts={} nic_q1_pkts={} "
                 "mempool_avail={} mempool_inuse={}\n",
                 total_bursts,
                 total_pkts,
                 prach_count.load(),
                 srs_count.load(),
                 other_count.load(),
                 peek_fail,
                 cuda_fail,
                 last_msgtyp,
                 last_pc_id,
                 q0_pkts,
                 q1_pkts,
                 mp_avail,
                 mp_inuse);
      last_log = now;
    }
  }
}
#endif

static bool port_init_host_only(const dpdk_port_config& config, ::rte_mempool* mem_pool, unsigned port_id)
{
  uint16_t nb_rxd = RX_RING_SIZE;
  uint16_t nb_txd = TX_RING_SIZE;

  if (::rte_eth_dev_is_valid_port(port_id) == 0) {
    fmt::print("DPDK - Invalid port id '{}'\n", port_id);
    return false;
  }

  ::rte_eth_dev_info dev_info;
  int                ret = ::rte_eth_dev_info_get(port_id, &dev_info);
  if (ret != 0) {
    fmt::print("DPDK - Error getting Ethernet device information: {}\n", port_id, ::strerror(-ret));
    return false;
  }

  ::rte_eth_conf port_conf = {};
  if (dev_info.tx_offload_capa & RTE_ETH_TX_OFFLOAD_MBUF_FAST_FREE) {
    port_conf.txmode.offloads |= RTE_ETH_TX_OFFLOAD_MBUF_FAST_FREE;
  }

  if (::rte_eth_dev_configure(port_id, 1, 1, &port_conf) != 0) {
    fmt::print("DPDK - Error configuring Ethernet device\n");
    return false;
  }

  if (::rte_eth_dev_set_mtu(port_id, config.mtu_size.value()) != 0) {
    uint16_t current_mtu;
    ::rte_eth_dev_get_mtu(port_id, &current_mtu);
    fmt::print("DPDK - Unable to configure MTU size to '{}' bytes, current MTU size is '{}' bytes\n",
               config.mtu_size,
               current_mtu);
    return false;
  }

  if (::rte_eth_dev_adjust_nb_rx_tx_desc(port_id, &nb_rxd, &nb_txd) != 0) {
    fmt::print("DPDK - Error configuring Ethernet device number of tx/rx descriptors\n");
    return false;
  }

  if (::rte_eth_rx_queue_setup(port_id, 0, nb_rxd, ::rte_eth_dev_socket_id(port_id), nullptr, mem_pool) < 0) {
    fmt::print("DPDK - Error configuring Rx queue\n");
    return false;
  }

  ::rte_eth_txconf txconf = dev_info.default_txconf;
  txconf.offloads         = port_conf.txmode.offloads;
  if (::rte_eth_tx_queue_setup(port_id, 0, nb_txd, ::rte_eth_dev_socket_id(port_id), &txconf) < 0) {
    fmt::print("DPDK - Error configuring Tx queue\n");
    return false;
  }

  if (::rte_eth_dev_start(port_id) < 0) {
    fmt::print("DPDK - Error starting Ethernet device\n");
    return false;
  }

  if (config.is_promiscuous_mode_enabled) {
    if (::rte_eth_promiscuous_enable(port_id) != 0) {
      fmt::print("DPDK - Error enabling promiscuous mode\n");
      return false;
    }
  }

  return true;
}

#ifdef ENABLE_GPU_FRONTHAUL
namespace ocudu {
namespace ether {
bool init_port_with_gpu(dpdk_port_context& ctx, const dpdk_port_config& config, unsigned port_id)
{
  uint16_t nb_rxd = RX_RING_SIZE;
  uint16_t nb_txd = TX_RING_SIZE;

  if (::rte_eth_dev_is_valid_port(port_id) == 0) {
    fmt::print("DPDK GPU - Invalid port id '{}'\n", port_id);
    return false;
  }
  ::rte_eth_dev_info dev_info;
  if (::rte_eth_dev_info_get(port_id, &dev_info) != 0) {
    fmt::print("DPDK GPU - Error getting Ethernet device information\n");
    return false;
  }

  ::rte_ether_addr mac{};
  ::rte_eth_macaddr_get(port_id, &mac);
  fmt::print("DPDK GPU - port {} driver='{}' socket={} max_rx_queues={} max_tx_queues={} "
             "mac={:02x}:{:02x}:{:02x}:{:02x}:{:02x}:{:02x}\n",
             port_id,
             dev_info.driver_name ? dev_info.driver_name : "?",
             ::rte_eth_dev_socket_id(port_id),
             dev_info.max_rx_queues, dev_info.max_tx_queues,
             mac.addr_bytes[0], mac.addr_bytes[1], mac.addr_bytes[2],
             mac.addr_bytes[3], mac.addr_bytes[4], mac.addr_bytes[5]);
  fmt::print("DPDK GPU - port {} mtu={} B nb_rxd={} nb_txd={} burst_size={} mbuf_pool_size={} "
             "huge_iova_mode={}\n",
             port_id,
             config.mtu_size.value(), RX_RING_SIZE, TX_RING_SIZE,
             MAX_BURST_SIZE, NUM_MBUFS,
             rte_eal_iova_mode() == RTE_IOVA_PA ? "PA" : "VA");

  ::rte_eth_conf port_conf = {};
  port_conf.rxmode.mq_mode = RTE_ETH_MQ_RX_NONE;
  if (dev_info.tx_offload_capa & RTE_ETH_TX_OFFLOAD_MBUF_FAST_FREE) {
    port_conf.txmode.offloads |= RTE_ETH_TX_OFFLOAD_MBUF_FAST_FREE;
  }
  if (::rte_eth_dev_configure(port_id, 2, 1, &port_conf) != 0) {
    fmt::print("DPDK GPU - Error configuring Ethernet device with 2 RX queues\n");
    return false;
  }

  if (::rte_eth_dev_set_mtu(port_id, config.mtu_size.value()) != 0) {
    uint16_t current_mtu;
    ::rte_eth_dev_get_mtu(port_id, &current_mtu);
    fmt::print("DPDK GPU - Unable to configure MTU '{}', current '{}'\n", config.mtu_size, current_mtu);
    return false;
  }
  if (::rte_eth_dev_adjust_nb_rx_tx_desc(port_id, &nb_rxd, &nb_txd) != 0) {
    fmt::print("DPDK GPU - Error adjusting rx/tx descriptors\n");
    return false;
  }

  hal::cuda::gpu_dpdk_mempool_config gcfg{};
  gcfg.gpu_dev_id     = config.gpu_rx.gpu_dev_id;
  gcfg.pool_name      = "OFH_GPU_MBUF_POOL";
  gcfg.data_room_size = MAX_BUFFER_SIZE + RTE_PKTMBUF_HEADROOM;
  gcfg.port_ids       = {static_cast<uint16_t>(port_id)};
  auto gpu_pool       = hal::cuda::gpu_dpdk_mempool::create(gcfg);
  if (!gpu_pool) {
    fmt::print("DPDK GPU - Failed to allocate GPU-backed mempool\n");
    return false;
  }
  ctx.gpu_mempool = std::move(gpu_pool);

  if (::rte_eth_rx_queue_setup(port_id, 0, nb_rxd, ::rte_eth_dev_socket_id(port_id), nullptr, ctx.mem_pool) < 0) {
    fmt::print("DPDK GPU - rx_queue_setup(host=0) failed\n");
    return false;
  }
  if (::rte_eth_rx_queue_setup(
          port_id, 1, nb_rxd, ::rte_eth_dev_socket_id(port_id), nullptr, ctx.gpu_mempool->mempool()) < 0) {
    fmt::print("DPDK GPU - rx_queue_setup(gpu=1) failed — does the PMD accept GPU extbuf?\n");
    return false;
  }

  ::rte_eth_txconf txconf = dev_info.default_txconf;
  txconf.offloads         = port_conf.txmode.offloads;
  if (::rte_eth_tx_queue_setup(port_id, 0, nb_txd, ::rte_eth_dev_socket_id(port_id), &txconf) < 0) {
    fmt::print("DPDK GPU - tx_queue_setup failed\n");
    return false;
  }

  if (::rte_eth_dev_start(port_id) < 0) {
    fmt::print("DPDK GPU - rte_eth_dev_start failed\n");
    return false;
  }
  if (config.is_promiscuous_mode_enabled) {
    ::rte_eth_promiscuous_enable(port_id);
  }

  ::rte_eth_link link{};
  if (::rte_eth_link_get_nowait(port_id, &link) == 0) {
    fmt::print("DPDK GPU - port {} link={} speed={} Mbps duplex={} autoneg={}{}\n",
               port_id,
               link.link_status == RTE_ETH_LINK_UP ? "UP" : "DOWN",
               link.link_speed,
               link.link_duplex == RTE_ETH_LINK_FULL_DUPLEX ? "full" : "half",
               link.link_autoneg == RTE_ETH_LINK_AUTONEG ? "yes" : "no",
               config.is_promiscuous_mode_enabled ? " promisc=on" : "");
  }

  bool                     used_per_eaxc = false;
  std::vector<::rte_flow*> rules         = install_gpu_steering_rules(
      static_cast<uint16_t>(port_id), config.gpu_rx.prach_eaxcs, 1, used_per_eaxc);
  if (rules.empty()) {
    fmt::print("DPDK GPU - Failed to install any steering rule; GPU queue will be idle\n");
    return false;
  }
  ctx.gpu_flow_rules = std::move(rules);
  fmt::print("DPDK GPU - steering mode = {} ({} rule(s))\n",
             used_per_eaxc ? "per-eAxC hardware filter" : "ethertype-only (sw filters per-eAxC)",
             ctx.gpu_flow_rules.size());

  ctx.gpu_rx = config.gpu_rx;
  ctx.gpu_listener_thread = std::thread(
      gpu_listener_loop,
      static_cast<uint16_t>(port_id),
      1,
      config.gpu_rx.prach_eaxcs,
      ctx.gpu_mempool->mempool(),
      std::ref(ctx.gpu_stop),
      std::ref(ctx.gpu_prach_frames),
      std::ref(ctx.gpu_srs_frames),
      std::ref(ctx.gpu_other_frames),
      config.gpu_rx.inline_pipeline,
      config.gpu_rx.iq_payload_offset_bytes,
      config.gpu_rx.srs_classify_enabled,
      config.gpu_rx.ul_eaxcs,
      config.gpu_rx.srs_inline_pipeline);

  fmt::print("DPDK GPU - port {} initialized with 2 RX queues (host=0, gpu=1, prach_eaxcs={} entries)\n",
             port_id,
             config.gpu_rx.prach_eaxcs.size());
  return true;
}
}
}
#endif

static void print_link_status(unsigned port_id)
{
  ::rte_eth_link link = {};

  if (::rte_eth_link_get(port_id, &link) < 0) {
    fmt::print("DPDK - Failed to retrieve port link status\n");
    return;
  }

  if (link.link_status != RTE_ETH_LINK_UP) {
    fmt::print("DPDK - Port {} link status is \"DOWN\" \n", port_id);
  }
}

static std::optional<int> parse_int(const std::string& value)
{
  int result{};
  auto [ptr, ec] = std::from_chars(value.data(), value.data() + value.size(), result);

  if (ec != std::errc() || ptr != (value.data() + value.size())) {
    return std::nullopt;
  }

  return result;
}

static std::optional<uint16_t> resolve_dpdk_port_id(const std::string& port_id)
{
  uint16_t dpdk_port_id;
  if (::rte_eth_dev_get_port_by_name(port_id.c_str(), &dpdk_port_id) == 0) {
    return dpdk_port_id;
  }
  if (auto result = parse_int(port_id); result && *result >= 0) {
    return result;
  }
  return std::nullopt;
}

std::shared_ptr<dpdk_port_context> dpdk_port_context::create(const dpdk_port_config& config)
{
  static ::rte_mempool* mem_pool = [&config]() {
    ::rte_mempool* pool = ::rte_pktmbuf_pool_create(
        "OFH_MBUF_POOL", NUM_MBUFS, MBUF_CACHE_SIZE, 0, (MAX_BUFFER_SIZE + RTE_PKTMBUF_HEADROOM), ::rte_socket_id());
    if (pool == nullptr) {
      ::rte_exit(EXIT_FAILURE, "DPDK - Unable to create the DPDK mbuf pool for port '%s'\n", config.id.c_str());
    }
    return pool;
  }();

  auto opt_port_id = resolve_dpdk_port_id(config.id);
  if (!opt_port_id) {
    ::rte_exit(EXIT_FAILURE,
               "DPDK - Unable to find an Ethernet port with device id '%s'. Make sure the device id is valid and "
               "is bound to DPDK\n",
               config.id.c_str());
  }
  uint16_t port_id = *opt_port_id;

  auto ctx = std::shared_ptr<dpdk_port_context>(new dpdk_port_context(config.id, port_id, mem_pool));

#ifdef ENABLE_GPU_FRONTHAUL
  if (config.gpu_rx.enabled) {
    if (!init_port_with_gpu(*ctx, config, port_id)) {
      ::rte_exit(EXIT_FAILURE, "DPDK - GPU-fronthaul init failed for port '%s'\n", config.id.c_str());
    }
  } else
#endif
  {
    if (!port_init_host_only(config, mem_pool, port_id)) {
      ::rte_exit(EXIT_FAILURE, "DPDK - Unable to initialize Ethernet port '%u'\n", port_id);
    }
  }

  if (config.is_link_status_check_enabled) {
    print_link_status(port_id);
  }
  return ctx;
}

dpdk_port_context::~dpdk_port_context()
{
  fmt::print("DPDK - Closing port '{}', id = '{}' ... ", port_id, dpdk_port_id);

#ifdef ENABLE_GPU_FRONTHAUL
  if (gpu_listener_thread.joinable()) {
    gpu_stop.store(true, std::memory_order_relaxed);
    gpu_listener_thread.join();
  }
  for (::rte_flow* h : gpu_flow_rules) {
    if (h != nullptr) {
      ::rte_flow_error err{};
      ::rte_flow_destroy(dpdk_port_id, h, &err);
    }
  }
  gpu_flow_rules.clear();
  gpu_mempool.reset();
#endif

  int ret = ::rte_eth_dev_stop(dpdk_port_id);
  if (ret != 0) {
    fmt::print("rte_eth_dev_stop: err '{}', port_id '{}'\n", ret, dpdk_port_id);
  }
  ret = ::rte_eth_dev_close(dpdk_port_id);
  if (ret != 0) {
    fmt::print("rte_eth_dev_close: err '{}', port_id '{}'\n", rte_errno, dpdk_port_id);
  }
  ::rte_mempool_free(mem_pool);

  fmt::print(" Done\n");
}
