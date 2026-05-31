// SPDX-FileCopyrightText: Copyright (C) 2021-2026 Software Radio Systems Limited
// SPDX-License-Identifier: BSD-3-Clause-Open-MPI

#pragma once

#include "ocudu/adt/static_vector.h"
#include "ocudu/ocudulog/ocudulog.h"
#include "ocudu/ofh/compression/compression_params.h"
#include "ocudu/ofh/ethernet/ethernet_mac_address.h"
#include "ocudu/ofh/ethernet/ethernet_receiver.h"
#include "ocudu/ofh/ethernet/ethernet_transmitter.h"
#include "ocudu/ofh/ofh_constants.h"
#include "ocudu/ofh/ofh_uplane_rx_symbol_notifier.h"
#include "ocudu/ofh/receiver/ofh_receiver_configuration.h"
#include "ocudu/ofh/transmitter/ofh_transmitter_configuration.h"
#include "ocudu/ran/bs_channel_bandwidth.h"
#include "ocudu/ran/cyclic_prefix.h"
#include "ocudu/ran/prach/prach_format_type.h"
#include "ocudu/ran/prach/restricted_set_config.h"
#include <string>

namespace ocudu {

class task_executor;

namespace ofh {

class error_notifier;

/// Open Fronthaul sector configuration.
struct sector_configuration {
  /// Radio sector identifier.
  unsigned sector_id;
  /// Ethernet interface name or identifier.
  std::string interface;
  /// Promiscuous mode flag.
  bool is_promiscuous_mode_enabled;
  /// Ethernet link status checking flag.
  bool is_link_status_check_enabled;
  /// MTU size.
  units::bytes mtu_size;
  /// Destination MAC address, corresponds to the Radio Unit MAC address.
  ether::mac_address mac_dst_address;
  /// Source MAC address, corresponds to the Distributed Unit MAC address.
  ether::mac_address mac_src_address;
  /// Tag control information field for C-Plane.
  std::optional<uint16_t> tci_cp;
  /// Tag control information field for U-Plane.
  std::optional<uint16_t> tci_up;

  /// DU transmission window timing parameters.
  tx_window_timing_parameters tx_window_timing_params;
  /// Reception window timing parameters.
  rx_window_timing_parameters rx_window_timing_params;

  /// Cyclic prefix.
  cyclic_prefix cp;
  /// Highest subcarrier spacing.
  subcarrier_spacing scs;
  /// Cell channel bandwidth.
  bs_channel_bandwidth bw;
  /// \brief RU operating bandwidth.
  ///
  /// Set this option when the operating bandwidth of the RU is larger than the configured bandwidth of the cell.
  bs_channel_bandwidth ru_operating_bw;

  /// PRACH eAxC.
  static_vector<unsigned, MAX_NOF_SUPPORTED_EAXC> prach_eaxc;
  /// Downlink eAxC.
  static_vector<unsigned, MAX_NOF_SUPPORTED_EAXC> dl_eaxc;
  /// Uplink eAxC.
  static_vector<unsigned, MAX_NOF_SUPPORTED_EAXC> ul_eaxc;

  /// Enables the Control-Plane PRACH message signalling.
  bool is_prach_control_plane_enabled = false;
  bool prach_rx_to_gpu = false;
  bool srs_rx_to_gpu   = false;
  /// Ignore the start symbol value received in the PRACH U-Plane packets.
  bool ignore_prach_start_symbol = false;
  /// If set to true, the payload size encoded in a eCPRI header is ignored.
  bool ignore_ecpri_payload_size_field = false;
  /// If set to true, the sequence id encoded in a eCPRI packet is ignored.
  bool ignore_ecpri_seq_id_field = false;
  /// If set to true, metrics are enabled in the sector.
  bool are_metrics_enabled = false;
  /// If set to true, logs late events as warnings, otherwise as info.
  bool enable_log_warnings_for_lates = true;
  /// Warn of unreceived Radio Unit frames status.
  warn_unreceived_ru_frames log_unreceived_ru_frames = warn_unreceived_ru_frames::after_traffic_detection;
  /// Uplink compression parameters.
  ru_compression_params ul_compression_params;
  /// Downlink compression parameters.
  ru_compression_params dl_compression_params;
  /// PRACH compression parameters.
  ru_compression_params prach_compression_params;
  /// Downlink static compression header flag.
  bool is_downlink_static_compr_hdr_enabled = true;
  /// Uplink static compression header flag.
  bool is_uplink_static_compr_hdr_enabled = true;
  /// IQ data scaling to be applied prior to Downlink data compression.
  float iq_scaling;
  /// C-Plane PRACH FFT size (to be used in Type 3 messages).
  ofh::cplane_fft_size c_plane_prach_fft_len = ofh::cplane_fft_size::fft_4096;
  /// \brief Number of slots the timing handler is notified in advance of the transmission time.
  ///
  /// Sets the maximum allowed processing delay in slots.
  unsigned max_processing_delay_slots;
  /// Downlink processing time in microseconds.
  std::chrono::microseconds dl_processing_time;
  /// Uplink request processing time in microseconds.
  std::chrono::microseconds ul_processing_time;
  /// Number of reception antennas.
  unsigned nof_antennas_ul;
  unsigned nof_prach_rx_ports = 1;
  float detection_threshold_margin = 1.0F;
  prach_format_type     prach_format                = prach_format_type::invalid;
  uint8_t               prach_zero_correlation_zone = 0;
  unsigned              prach_root_sequence_index   = 0;
  restricted_set_config prach_restricted_set        = restricted_set_config::UNRESTRICTED;

  /// Indicates if DPDK should be used by the underlying implementation.
  bool uses_dpdk;
  /// Optional TDD configuration.
  std::optional<tdd_ul_dl_config_common> tdd_config;
};

/// Open Fronthaul sector dependencies.
struct sector_dependencies {
  /// Logger.
  ocudulog::basic_logger* logger = nullptr;
  /// Error notifier.
  error_notifier* err_notifier = nullptr;
  /// Downlink task executor.
  task_executor* downlink_executor = nullptr;
  /// Message transmitter and receiver task executor.
  task_executor* txrx_executor = nullptr;
  /// Uplink task executor.
  task_executor* uplink_executor = nullptr;
  /// User-Plane received symbol notifier.
  uplane_rx_symbol_notifier* notifier = nullptr;
  /// Optional Ethernet transmitter.
  std::optional<std::unique_ptr<ether::transmitter>> eth_transmitter;
  /// Optional Ethernet receiver.
  std::optional<std::unique_ptr<ether::receiver>> eth_receiver;
};

} // namespace ofh
} // namespace ocudu
