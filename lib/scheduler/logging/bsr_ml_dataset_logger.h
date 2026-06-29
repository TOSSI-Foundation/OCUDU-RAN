// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#pragma once

#include <cstdint>
#include <string>

namespace ocudu {

namespace bsr_ml_dataset {

enum class bsr_trigger_type : uint8_t {

  unknown = 0,

  regular = 1,

  periodic = 2,

  padding = 3
};

struct bsr_sample {

  uint32_t slot;
  uint16_t sfn;
  uint8_t  subframe;
  uint8_t  slot_in_frame;
  uint8_t  numerology;

  uint16_t rnti;
  uint16_t ue_index;

  uint8_t  bsr_format;
  uint32_t bsr_total_bytes;
  uint32_t lcg0_bytes;
  uint32_t lcg1_bytes;
  uint32_t lcg2_bytes;
  uint32_t lcg3_bytes;

  uint8_t  wideband_cqi;
  uint8_t  dl_ri;
  uint8_t  ul_ri;
  float    pusch_snr_db;
  float    pusch_avg_sinr_db;

  double dl_brate_kbps;

  uint32_t pending_ul_bytes;

  double   ul_brate_kbps;
  uint64_t ul_tb_bytes;
  unsigned nof_ul_grants;

  unsigned sr_count;

  bsr_trigger_type trigger_type = bsr_trigger_type::unknown;

  bool has_interarrival = false;

  uint32_t interarrival_slots = 0;

  bool has_last_ul_over_air_delay = false;

  uint32_t last_ul_over_air_delay_slots = 0;

  bool has_predicted_periodicity = false;

  double predicted_interarrival_slots = 0.0;

  uint16_t predicted_periodicity_subframes = 0;

};

void configure(bool               enabled,
               const std::string& output_dir,
               const std::string& scenario,
               unsigned           periodic_bsr_timer_subframes,
               unsigned           retx_bsr_timer_subframes);

bool is_enabled();

void log_bsr_sample(const bsr_sample& s);

void classify_bsr_trigger(uint16_t         ue_index,
                          uint32_t         slot,
                          uint8_t          numerology,
                          uint8_t          bsr_format,
                          const uint32_t   lcg_bytes[4],
                          bsr_trigger_type& trigger_type,
                          bool&            has_interarrival,
                          uint32_t&        interarrival_slots,
                          bool&            has_predicted_periodicity,
                          double&          predicted_interarrival_slots,
                          uint16_t&        predicted_periodicity_subframes);

void record_ul_grant(uint16_t ue_index, uint8_t harq_id, uint32_t slot, bool new_data);

void record_ul_crc_success(uint16_t ue_index, uint8_t harq_id, uint32_t slot, uint8_t numerology);

void remove_ue(uint16_t ue_index);

unsigned get_recommended_periodic_bsr_timer(uint16_t ue_index);

}
}
