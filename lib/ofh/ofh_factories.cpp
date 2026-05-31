// SPDX-FileCopyrightText: Copyright (C) 2021-2026 Software Radio Systems Limited
// SPDX-License-Identifier: BSD-3-Clause-Open-MPI

#include "ocudu/ofh/ofh_factories.h"
#include "ofh_sector_impl.h"
#include "receiver/ofh_receiver_factories.h"
#include "receiver/ofh_sequence_id_checker_impl.h"
#include "timing/ofh_timing_manager_impl.h"
#include "transmitter/ofh_transmitter_factories.h"
#include "ocudu/ofh/ethernet/ethernet_factories.h"

#ifdef DPDK_FOUND
#include "ocudu/ofh/ethernet/dpdk/dpdk_ethernet_factories.h"
#endif

#ifdef ENABLE_GPU_FRONTHAUL
#include "ocudu/hal/cuda/inline_prach_pipeline.h"
#include "ocudu/hal/cuda/inline_srs_pipeline.h"
#include "ocudu/ran/band_helper.h"
#include "ocudu/ran/prach/prach_format_type.h"
#include "ocudu/ran/prach/prach_subcarrier_spacing.h"
#include "ocudu/ran/prach/restricted_set_config.h"
#include "ocudu/ran/resource_block.h"
#include "ocudu/ran/slot_point.h"
#include "ocudu/ran/subcarrier_spacing.h"
#include "ocudu/ofh/compression/compression_params.h"
#include "ocudu/support/srs_result_tap.h"
#include "fmt/format.h"
#include <cstdio>
#include <cstdlib>
#include <unistd.h>
#endif

using namespace ocudu;
using namespace ofh;

#ifdef ENABLE_GPU_FRONTHAUL
namespace {
bool prach_bench_logs()
{
  static const bool on = std::getenv("OCUDU_PRACH_BENCH") != nullptr;
  return on;
}
const char* ansi_green()
{
  static const char* s = ::isatty(::fileno(stderr)) ? "\033[1;32m" : "";
  return s;
}
const char* ansi_reset()
{
  static const char* s = ::isatty(::fileno(stderr)) ? "\033[0m" : "";
  return s;
}
} // namespace
#endif

std::unique_ptr<sequence_id_checker> ofh::create_sequence_id_checker()
{
  return std::make_unique<sequence_id_checker_impl>();
}

std::unique_ptr<timing_manager> ocudu::ofh::create_ofh_timing_manager(const controller_config& config,
                                                                      ocudulog::basic_logger&  logger,
                                                                      task_executor&           executor)
{
  realtime_worker_cfg rt_cfg = {config.cp,
                                config.scs,
                                config.gps_Alpha,
                                config.gps_Beta,
                                config.enable_log_warnings_for_lates,
                                config.enable_busy_waiting};

  return std::make_unique<timing_manager_impl>(logger, executor, rt_cfg);
}

static receiver_config generate_receiver_config(const sector_configuration& config)
{
  receiver_config rx_config;
  rx_config.sector                             = config.sector_id;
  rx_config.ru_operating_bw                    = config.ru_operating_bw;
  rx_config.scs                                = config.scs;
  rx_config.cp                                 = config.cp;
  rx_config.is_uplink_static_compr_hdr_enabled = config.is_uplink_static_compr_hdr_enabled;
  rx_config.prach_compression_params           = config.prach_compression_params;
  rx_config.ul_compression_params              = config.ul_compression_params;
  rx_config.is_prach_control_plane_enabled     = config.is_prach_control_plane_enabled;
  rx_config.ignore_prach_start_symbol          = config.ignore_prach_start_symbol;
  rx_config.ignore_ecpri_payload_size_field    = config.ignore_ecpri_payload_size_field;
  rx_config.ignore_ecpri_seq_id_field          = config.ignore_ecpri_seq_id_field;
  rx_config.are_metrics_enabled                = config.are_metrics_enabled;
  rx_config.log_unreceived_ru_frames           = config.log_unreceived_ru_frames;
  rx_config.enable_log_warnings_for_lates      = config.enable_log_warnings_for_lates;

  // For the rx eAxCs, configure only those that will be used, so the other eAxCs can be discarded as soon as possible.
  rx_config.prach_eaxc.assign(config.prach_eaxc.begin(), config.prach_eaxc.begin() + config.nof_antennas_ul);
  rx_config.ul_eaxc.assign(config.ul_eaxc.begin(), config.ul_eaxc.begin() + config.nof_antennas_ul);

  // In rx, dst and src addresses are swapped.
  rx_config.mac_dst_address  = config.mac_src_address;
  rx_config.mac_src_address  = config.mac_dst_address;
  rx_config.tci              = config.tci_up;
  rx_config.rx_timing_params = config.rx_window_timing_params;
  rx_config.prach_rx_to_gpu  = config.prach_rx_to_gpu;
  rx_config.srs_rx_to_gpu    = config.srs_rx_to_gpu;

  return rx_config;
}

static transmitter_config generate_transmitter_config(const sector_configuration& sector_cfg)
{
  transmitter_config tx_config;

  tx_config.sector                               = sector_cfg.sector_id;
  tx_config.bw                                   = sector_cfg.bw;
  tx_config.scs                                  = sector_cfg.scs;
  tx_config.cp                                   = sector_cfg.cp;
  tx_config.dl_eaxc                              = sector_cfg.dl_eaxc;
  tx_config.ul_eaxc                              = sector_cfg.ul_eaxc;
  tx_config.prach_eaxc                           = sector_cfg.prach_eaxc;
  tx_config.is_prach_cp_enabled                  = sector_cfg.is_prach_control_plane_enabled;
  tx_config.mac_dst_address                      = sector_cfg.mac_dst_address;
  tx_config.mac_src_address                      = sector_cfg.mac_src_address;
  tx_config.tci_cp                               = sector_cfg.tci_cp;
  tx_config.tci_up                               = sector_cfg.tci_up;
  tx_config.interface                            = sector_cfg.interface;
  tx_config.is_promiscuous_mode_enabled          = sector_cfg.is_promiscuous_mode_enabled;
  tx_config.mtu_size                             = sector_cfg.mtu_size;
  tx_config.ru_working_bw                        = sector_cfg.ru_operating_bw;
  tx_config.tx_timing_params                     = sector_cfg.tx_window_timing_params;
  tx_config.dl_compr_params                      = sector_cfg.dl_compression_params;
  tx_config.ul_compr_params                      = sector_cfg.ul_compression_params;
  tx_config.prach_compr_params                   = sector_cfg.prach_compression_params;
  tx_config.is_downlink_static_compr_hdr_enabled = sector_cfg.is_downlink_static_compr_hdr_enabled;
  tx_config.is_uplink_static_compr_hdr_enabled   = sector_cfg.is_uplink_static_compr_hdr_enabled;
  tx_config.iq_scaling                           = sector_cfg.iq_scaling;
  tx_config.dl_processing_time                   = sector_cfg.dl_processing_time;
  tx_config.ul_processing_time                   = sector_cfg.ul_processing_time;
  tx_config.tdd_config                           = sector_cfg.tdd_config;
  tx_config.uses_dpdk                            = sector_cfg.uses_dpdk;
  tx_config.are_metrics_enabled                  = sector_cfg.are_metrics_enabled;
  tx_config.c_plane_prach_fft_len                = sector_cfg.c_plane_prach_fft_len;
  tx_config.enable_log_warnings_for_lates        = sector_cfg.enable_log_warnings_for_lates;

  return tx_config;
}

#ifdef DPDK_FOUND
static std::pair<std::unique_ptr<ether::transmitter>, std::unique_ptr<ether::receiver>>
create_dpdk_txrx(const sector_configuration& sector_cfg, task_executor& rx_executor, ocudulog::basic_logger& logger)
{
  ether::transmitter_config eth_cfg;
  eth_cfg.interface                    = sector_cfg.interface;
  eth_cfg.is_promiscuous_mode_enabled  = sector_cfg.is_promiscuous_mode_enabled;
  eth_cfg.is_link_status_check_enabled = sector_cfg.is_link_status_check_enabled;
  eth_cfg.are_metrics_enabled          = sector_cfg.are_metrics_enabled;
  eth_cfg.mtu_size                     = sector_cfg.mtu_size;
  eth_cfg.mac_dst_address              = sector_cfg.mac_dst_address;
  eth_cfg.enable_gpu_rx_queue          = sector_cfg.prach_rx_to_gpu;
  eth_cfg.enable_srs_classification    = sector_cfg.srs_rx_to_gpu;
  if (sector_cfg.prach_rx_to_gpu) {
    eth_cfg.gpu_prach_eaxcs.reserve(sector_cfg.prach_eaxc.size());
    for (unsigned e : sector_cfg.prach_eaxc) {
      eth_cfg.gpu_prach_eaxcs.push_back(static_cast<uint16_t>(e));
    }
    eth_cfg.gpu_iq_payload_offset_bytes = sector_cfg.is_uplink_static_compr_hdr_enabled ? 34u : 36u;

#ifdef ENABLE_GPU_FRONTHAUL
    hal::cuda::inline_prach_pipeline::config pcfg{};
    pcfg.nof_prbs_per_packet = 12;
    pcfg.k_bar               = 2;
    pcfg.prb_bytes           = (sector_cfg.prach_compression_params.data_width * 12 * 2 +
                                (sector_cfg.prach_compression_params.type == compression_type::BFP ? 8 : 0) + 7) /
                               8;
    pcfg.data_width          = sector_cfg.prach_compression_params.data_width;
    pcfg.quantizer_gain      = 32767.0F;
    pcfg.L_ra                = 139;
    pcfg.nof_prach_eaxc      = sector_cfg.prach_eaxc.size();
    pcfg.nof_prach_symbols   = 12;
    pcfg.detector_inline_cfg = {};
    pcfg.detector_cfg.root_sequence_index   = sector_cfg.prach_root_sequence_index;
    pcfg.detector_cfg.format                = sector_cfg.prach_format;
    pcfg.detector_cfg.restricted_set        = sector_cfg.prach_restricted_set;
    pcfg.detector_cfg.zero_correlation_zone = sector_cfg.prach_zero_correlation_zone;
    pcfg.detector_cfg.start_preamble_index  = 0;
    pcfg.detector_cfg.nof_preamble_indices  = 64;
    pcfg.detector_cfg.ra_scs                = prach_subcarrier_spacing::kHz30;
    pcfg.detector_cfg.nof_rx_ports               = sector_cfg.nof_prach_rx_ports;
    pcfg.detector_cfg.slot                       = slot_point(to_numerology_value(sector_cfg.scs), 0);
    pcfg.detector_cfg.detection_threshold_margin = sector_cfg.detection_threshold_margin;
    pcfg.on_result_cb = [](const prach_detection_result& r, uint32_t slot_id) {
      if (!prach_bench_logs() || r.preambles.empty()) {
        return;
      }
      for (const auto& p : r.preambles) {
        fmt::print(stderr,
                   "[prach_pipeline backend=gpu] slot={} preamble idx={} metric={:.3f} "
                   "ta={}ns power={:.2f}dB\n",
                   slot_id,
                   p.preamble_index,
                   p.detection_metric,
                   static_cast<long>(p.time_advance.to_seconds() * 1e9),
                   p.preamble_power_dB);
      }
    };
    eth_cfg.inline_pipeline = hal::cuda::inline_prach_pipeline::create(pcfg);
    if (!eth_cfg.inline_pipeline) {
      fmt::print(stderr,
                 "[ofh_factories] WARN: inline_prach_pipeline::create returned null; "
                 "prach_rx_to_gpu will run in Phase-5-only mode (classify + counter, no detect)\n");
    } else {
      fmt::print(stderr,
                 "{}[ofh_factories] Sector#{} inline GPU PRACH pipeline active "
                 "(nof_prach_eaxc={} nof_rx_ports={} prach_compr=BFP{} iq_offset={}){}\n",
                 ansi_green(),
                 sector_cfg.sector_id,
                 pcfg.nof_prach_eaxc,
                 pcfg.detector_cfg.nof_rx_ports,
                 pcfg.data_width,
                 eth_cfg.gpu_iq_payload_offset_bytes,
                 ansi_reset());
    }

    if (sector_cfg.srs_rx_to_gpu) {
      eth_cfg.gpu_ul_eaxcs.reserve(sector_cfg.ul_eaxc.size());
      for (unsigned e : sector_cfg.ul_eaxc) {
        eth_cfg.gpu_ul_eaxcs.push_back(static_cast<uint16_t>(e));
      }

      const unsigned cell_nof_prbs =
          band_helper::get_n_rbs_from_bw(sector_cfg.bw, sector_cfg.scs, frequency_range::FR1);
      const unsigned nof_srs_rx_ports = std::min<unsigned>(sector_cfg.nof_antennas_ul, 4u);
      const unsigned max_seq_len      = cell_nof_prbs * NOF_SUBCARRIERS_PER_RB / 2u;

      hal::cuda::inline_srs_pipeline::config srs_pcfg{};
      srs_pcfg.prb_bytes      = (sector_cfg.ul_compression_params.data_width * 12 * 2 +
                                 (sector_cfg.ul_compression_params.type == compression_type::BFP ? 8 : 0) + 7) /
                                8;
      srs_pcfg.data_width          = sector_cfg.ul_compression_params.data_width;
      srs_pcfg.quantizer_gain      = 32767.0F;
      srs_pcfg.cell_nof_prbs       = cell_nof_prbs;
      srs_pcfg.numerology          = to_numerology_value(sector_cfg.scs);
      srs_pcfg.max_nof_rx_ports    = nof_srs_rx_ports;
      srs_pcfg.max_nof_symbols     = 4;
      srs_pcfg.max_sequence_length = max_seq_len;

      srs_pcfg.on_result_cb = [](const srs_estimator_result& r, uint32_t slot_id) {
        srs_result_tap::publish(slot_id, r);
      };

      eth_cfg.srs_inline_pipeline = hal::cuda::inline_srs_pipeline::create(srs_pcfg);
      if (!eth_cfg.srs_inline_pipeline) {
        fmt::print(stderr,
                   "[ofh_factories] WARN: inline_srs_pipeline::create returned null; srs_rx_to_gpu "
                   "will run in Phase-3a mode (classify + counter, no estimate)\n");
      } else {
        fmt::print(stderr,
                   "{}[ofh_factories] Sector#{} inline GPU SRS pipeline active "
                   "(nof_rx_ports={} ul_eaxcs={} cell_prbs={} max_seq={} ul_compr=BFP{}){}\n",
                   ansi_green(),
                   sector_cfg.sector_id,
                   nof_srs_rx_ports,
                   eth_cfg.gpu_ul_eaxcs.size(),
                   cell_nof_prbs,
                   max_seq_len,
                   srs_pcfg.data_width,
                   ansi_reset());
      }
    }
#endif
  }

  return ether::create_dpdk_txrx(eth_cfg, rx_executor, logger);
}
#endif

static std::pair<std::unique_ptr<ether::transmitter>, std::unique_ptr<ether::receiver>>
create_socket_txrx(const sector_configuration& sector_cfg, task_executor& rx_executor, ocudulog::basic_logger& logger)
{
  auto eth_receiver_config = ether::receiver_config{
      sector_cfg.interface, sector_cfg.is_promiscuous_mode_enabled, sector_cfg.are_metrics_enabled};

  auto rx = ether::create_receiver(eth_receiver_config, rx_executor, logger);

  ether::transmitter_config eth_cfg;
  eth_cfg.interface                   = sector_cfg.interface;
  eth_cfg.is_promiscuous_mode_enabled = sector_cfg.is_promiscuous_mode_enabled;
  eth_cfg.are_metrics_enabled         = sector_cfg.are_metrics_enabled;
  eth_cfg.mtu_size                    = sector_cfg.mtu_size;
  eth_cfg.mac_dst_address             = sector_cfg.mac_dst_address;
  auto tx                             = ether::create_transmitter(eth_cfg, logger);

  return {std::move(tx), std::move(rx)};
}

static std::pair<std::unique_ptr<ether::transmitter>, std::unique_ptr<ether::receiver>>
create_txrx(const sector_configuration&                        sector_cfg,
            std::optional<std::unique_ptr<ether::transmitter>> eth_transmitter,
            std::optional<std::unique_ptr<ether::receiver>>    eth_receiver,
            task_executor&                                     eth_rx_executor,
            ocudulog::basic_logger&                            logger)
{
  if (eth_transmitter && eth_receiver) {
    // Do not proceed if both optionals are provided.
    return {std::move(*eth_transmitter), std::move(*eth_receiver)};
  }

#ifdef DPDK_FOUND
  auto eth_txrx = (sector_cfg.uses_dpdk) ? create_dpdk_txrx(sector_cfg, eth_rx_executor, logger)
                                         : create_socket_txrx(sector_cfg, eth_rx_executor, logger);
#else
  auto eth_txrx = create_socket_txrx(sector_cfg, eth_rx_executor, logger);
#endif
  if (eth_transmitter) {
    eth_txrx.first = std::move(*eth_transmitter);
  }
  if (eth_receiver) {
    eth_txrx.second = std::move(*eth_receiver);
  }
  return eth_txrx;
}

std::unique_ptr<sector> ocudu::ofh::create_ofh_sector(const sector_configuration& sector_cfg,
                                                      sector_dependencies&&       sector_deps)
{
  unsigned repository_size = calculate_repository_size(sector_cfg.scs, sector_cfg.max_processing_delay_slots * 4);

  auto cp_repo                      = std::make_shared<uplink_cplane_context_repository>(repository_size);
  auto prach_cp_repo                = std::make_shared<uplink_cplane_context_repository>(repository_size);
  auto ul_prach_repo                = std::make_shared<prach_context_repository>(repository_size);
  auto ul_data_repo                 = std::make_shared<uplink_context_repository>(repository_size);
  auto ul_grid_symbol_notified_repo = std::make_shared<uplink_notified_grid_symbol_repository>(repository_size);

  // Build the ethernet txrx.
  auto eth_txrx = create_txrx(sector_cfg,
                              std::move(sector_deps.eth_transmitter),
                              std::move(sector_deps.eth_receiver),
                              *sector_deps.txrx_executor,
                              *sector_deps.logger);

  ether::transmitter& eth_transmitter = *eth_txrx.first;
  ether::receiver&    eth_receiver    = *eth_txrx.second;

  // Build the OFH receiver.
  auto rx_config = generate_receiver_config(sector_cfg);
  auto receiver  = create_receiver(rx_config,
                                  *sector_deps.logger,
                                  *sector_deps.uplink_executor,
                                  std::move(eth_txrx.second),
                                  sector_deps.notifier,
                                  ul_prach_repo,
                                  ul_data_repo,
                                  cp_repo,
                                  prach_cp_repo,
                                  ul_grid_symbol_notified_repo);

  ocudu_assert(sector_deps.err_notifier, "Invalid error notifier");

  // Build the OFH transmitter.
  auto tx_config   = generate_transmitter_config(sector_cfg);
  auto transmitter = create_transmitter(tx_config,
                                        *sector_deps.logger,
                                        *sector_deps.txrx_executor,
                                        *sector_deps.downlink_executor,
                                        *sector_deps.err_notifier,
                                        std::move(eth_txrx.first),
                                        ul_prach_repo,
                                        ul_data_repo,
                                        cp_repo,
                                        prach_cp_repo,
                                        ul_grid_symbol_notified_repo);

  return std::make_unique<sector_impl>(sector_impl_config{sector_cfg.sector_id, sector_cfg.are_metrics_enabled},
                                       sector_impl_dependencies{std::move(receiver),
                                                                std::move(transmitter),
                                                                std::move(ul_data_repo),
                                                                std::move(ul_prach_repo),
                                                                eth_transmitter,
                                                                eth_receiver});
}
