// SPDX-FileCopyrightText: Copyright (C) 2021-2026 Software Radio Systems Limited
// SPDX-License-Identifier: BSD-3-Clause-Open-MPI
// Portions of this file may implement 3GPP specifications, which may be subject to additional licensing requirements.

#pragma once

#include "ocudu/ran/rnti.h"
#include "ocudu/ran/slot_point.h"
#include <cstdint>
#include <string>

namespace ocudu {

/// \brief Self-labeled UL link-adaptation (MCS) dataset logger.
///
/// Captures one training row per decoded PUSCH at CRC time: channel state and grant parameters (features) joined with
/// the CRC outcome (label). Configured from the scheduler ml_mcs.dataset_logging config block.
namespace ml_la_dataset {

/// One UL PUSCH training sample. All values are taken at CRC-indication time from the HARQ grant params and the UE
/// channel-state manager. Optional PHY measurements (sinr/rsrp/ta) come from the CRC PDU and may be absent.
struct ul_sample {
  // --- keys / context ---
  uint32_t slot;       ///< PUSCH slot (system slot index).
  uint16_t rnti;       ///< UE C-RNTI.
  uint16_t ue_index;   ///< DU UE index.
  uint8_t  harq_id;    ///< UL HARQ process id.
  uint8_t  nof_retxs;  ///< Number of reTxs already done for this HARQ (0 == newTx).

  // --- action (what the scheduler chose) ---
  uint8_t  mcs;         ///< MCS index used for this transmission.
  uint8_t  mcs_table;   ///< PUSCH MCS table enum value.
  uint32_t tbs_bytes;   ///< Transport block size in bytes.
  uint16_t nof_prbs;    ///< Number of PRBs allocated.
  uint8_t  nof_symbols; ///< Number of OFDM symbols.
  uint8_t  nof_layers;  ///< Number of spatial layers.
  int16_t  olla_mcs;    ///< OLLA-suggested MCS (-1 if not available).

  // --- channel state (features the model uses) ---
  uint8_t wideband_cqi;     ///< Latest reported wideband CQI.
  float   pusch_snr_db;     ///< Latest PUSCH SNR estimate (dB).
  float   pusch_avg_sinr_db;///< EWMA of PUSCH SINR (dB).
  float   ul_snr_offset_db; ///< Current UL OLLA SNR offset (dB).

  // --- PHY measurements at RX (optional; NaN/sentinel when absent) ---
  float    ul_sinr_db;   ///< CRC PDU SINR (dB), NaN if absent.
  float    ul_rsrp_dbfs; ///< CRC PDU RSRP (dBFS), NaN if absent.
  int32_t  ta_ns;        ///< Timing advance offset (ns), INT32_MIN if absent.

  // === Extended KPIs (collected so future ML use cases need no recollection) ===

  // --- spatial / CSI (use case: CSI prediction, MIMO link adaptation) ---
  uint8_t nof_dl_layers;   ///< Recommended DL rank (RI) from CSI.
  uint8_t nof_ul_layers;   ///< Recommended UL rank (from SRS).
  int16_t csi_ri;          ///< Latest CSI report RI (-1 if none).
  int16_t csi_cri;         ///< Latest CSI report CRI (-1 if none).
  int32_t slots_since_srs; ///< Slots since last aperiodic SRS (-1 if never). Channel-meas freshness.

  // --- link-adaptation / OLLA full state (use case: MCS, A/B vs OLLA) ---
  float   dl_cqi_offset_db;  ///< DL OLLA CQI offset (dB).
  float   effective_cqi;     ///< OLLA-adjusted effective CQI.
  uint8_t ul_olla_enabled;   ///< 1 if UL OLLA active.
  uint8_t dl_olla_enabled;   ///< 1 if DL OLLA active.

  // --- HARQ / queue occupancy (use case: anomaly detection, scheduler policy) ---
  uint32_t ul_bytes_in_harq;   ///< Total UL bytes awaiting ACK across HARQs (in-flight queue depth).
  uint16_t nof_empty_ul_harqs; ///< Free UL HARQ processes (congestion signal).
  uint16_t nof_ul_harqs;       ///< Configured UL HARQ processes.
  uint8_t  max_nof_ul_retxs;   ///< Max reTxs allowed for this HARQ.
  uint8_t  ndi;                ///< New-data indicator of this HARQ.

  // --- grant / slicing context (use case: scheduler policy, slicing) ---
  int16_t slice_id;     ///< RAN slice id (-1 if none).
  uint8_t dci_cfg_type; ///< UL DCI config type enum (f0_0 / f0_1 / tc_rnti).

  // --- timing decomposition (use case: TDD-pattern features, traffic prediction) ---
  uint16_t sfn;           ///< System frame number (0..1023).
  uint8_t  slot_in_frame; ///< Slot index within the frame.
  uint8_t  subframe;      ///< Subframe index (0..9), i.e. ms within frame.
  uint8_t  numerology;    ///< SCS numerology (0..4).

  // --- state flags (use case: anomaly detection, filtering) ---
  uint8_t is_fallback; ///< 1 if UE in fallback (SRB-only) mode.

  // --- ML predictor verdict (use case: per-PUSCH predicted-vs-observed audit) ---
  uint8_t ml_in_control; ///< 1 if the ML model (not OLLA) chose `mcs`.
  float   ml_p_succ;     ///< Model's predicted P(crc_success) for the MCS that flew (NaN if model unavailable).

  // --- label ---
  uint8_t crc_success; ///< 1 = TB decoded OK, 0 = CRC failed.
};

/// Configures the logger from the ml_mcs.dataset_logging config. When enabled, opens a timestamped CSV
/// inside output_dir. Call once before logging; later calls are ignored.
void configure(bool enabled, const std::string& output_dir, const std::string& scenario);

/// Returns true if the dataset logger is enabled and an output file is open.
bool is_enabled();

/// Appends one UL sample to the dataset file. No-op if the logger is disabled. Thread-safe.
void log_ul_sample(const ul_sample& s);

} // namespace ml_la_dataset
} // namespace ocudu
