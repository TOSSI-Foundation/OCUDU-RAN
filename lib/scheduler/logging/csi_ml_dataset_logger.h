#pragma once

#include <cstdint>
#include <string>

namespace ocudu {

namespace csi_ml_dataset {

struct csi_sample {

  uint32_t slot;
  uint16_t sfn;
  uint8_t  subframe;
  uint8_t  slot_in_frame;
  uint8_t  numerology;

  uint16_t rnti;
  uint16_t ue_index;

  uint8_t valid;

  uint8_t wideband_cqi;

  int16_t dl_ri;

  int16_t dl_cri;

  float dl_cqi_offset_db;

  float effective_cqi;

  uint8_t dl_olla_enabled;

  bool  has_window;
  float window[4];

  uint8_t pred_kind;
  float   pred_value;
};

struct kpi_sample {

  uint32_t slot;
  uint16_t rnti;
  uint16_t ue_index;

  double dl_brate_kbps;

  uint32_t dl_nof_ok;
  uint32_t dl_nof_nok;

  uint8_t dl_mcs;

  float mean_cqi;

  float dl_ri;
};

void configure(bool enabled, const std::string& output_dir, const std::string& scenario);

bool is_enabled();

void log_csi_sample(const csi_sample& s);

void remove_ue(uint16_t ue_index);

void log_kpi_sample(const kpi_sample& s);

bool is_kpi_enabled();

}
}
