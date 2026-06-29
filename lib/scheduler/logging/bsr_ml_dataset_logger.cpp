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

#include "bsr_ml_dataset_logger.h"
#include "../support/bsr_periodicity_predictor.h"
#include "fmt/format.h"
#include <cstdio>
#include <ctime>
#include <deque>
#include <mutex>
#include <string>
#include <sys/stat.h>
#include <sys/types.h>
#include <unordered_map>

static void mkdir_p(const std::string& path)
{
  for (std::size_t pos = 1; pos <= path.size(); ++pos) {
    if (pos == path.size() || path[pos] == '/') {
      ::mkdir(path.substr(0, pos).c_str(), 0775);
    }
  }
}

using namespace ocudu;
using namespace ocudu::bsr_ml_dataset;

namespace {

constexpr unsigned MAX_NOF_LCGS_TRACKED = 4;

constexpr unsigned MAX_NOF_UL_HARQS = 32;

constexpr uint64_t NOF_SFNS = 1024;

constexpr uint64_t NOF_SUBFRAMES_PER_FRAME = 10;

uint32_t wrap_forward_distance(uint32_t from, uint32_t to, uint8_t numerology)
{
  const uint64_t modulus = NOF_SFNS * NOF_SUBFRAMES_PER_FRAME * (uint64_t{1} << numerology);
  const uint64_t f       = static_cast<uint64_t>(from) % modulus;
  const uint64_t t       = static_cast<uint64_t>(to) % modulus;
  return static_cast<uint32_t>((t + modulus - f) % modulus);
}

struct bsr_trigger_tracker {

  bool has_periodic_reset = false;

  uint32_t periodic_reset_slot = 0;

  bool has_retx_reset = false;

  uint32_t retx_reset_slot = 0;

  uint32_t last_lcg_bytes[MAX_NOF_LCGS_TRACKED] = {};

  bool has_last_lcg_bytes = false;

  uint32_t last_regular_slot = 0;

  bool has_last_regular_slot = false;

  std::deque<double> interarrival_window;

  unsigned last_recommended_periodic_bsr_timer = 0;
};

struct ul_delay_tracker {

  uint32_t pending_sched_slot[MAX_NOF_UL_HARQS] = {};

  bool has_pending_sched[MAX_NOF_UL_HARQS] = {};

  uint32_t last_delay_slots = 0;

  bool has_last_delay = false;
};

class bsr_dataset_writer
{
public:
  static bsr_dataset_writer& instance()
  {
    static bsr_dataset_writer inst;
    return inst;
  }

  void configure(bool                en,
                 const std::string&  output_dir,
                 const std::string&  scenario_tag,
                 unsigned            periodic_bsr_timer_subframes_,
                 unsigned            retx_bsr_timer_subframes_)
  {
    if (configured) {
      return;
    }
    configured                      = true;
    periodic_bsr_timer_subframes    = periodic_bsr_timer_subframes_;
    retx_bsr_timer_subframes        = retx_bsr_timer_subframes_;
    if (!en) {
      return;
    }
    if (!scenario_tag.empty()) {
      scenario = scenario_tag;
    }
    std::string dir = output_dir.empty() ? std::string("ml/datasets") : output_dir;
    mkdir_p(dir);
    std::string out  = fmt::format("{}/bsr_ml_dataset_{}.csv", dir, timestamp_now());
    const char* path = out.c_str();
    file             = std::fopen(path, "w");
    if (file == nullptr) {
      std::fprintf(stderr, "[bsr_ml_dataset] Failed to open '%s' for writing. BSR dataset logging disabled.\n", path);
      return;
    }

    std::fputs("slot,sfn,subframe,slot_in_frame,numerology,"
               "rnti,ue_index,"
               "bsr_format,bsr_total_bytes,lcg0_bytes,lcg1_bytes,lcg2_bytes,lcg3_bytes,"
               "wideband_cqi,dl_ri,ul_ri,pusch_snr_db,pusch_avg_sinr_db,"
               "dl_brate_kbps,"
               "pending_ul_bytes,"
               "ul_brate_kbps,ul_tb_bytes,nof_ul_grants,"
               "sr_count,"
               "bsr_trigger_type,has_interarrival,interarrival_slots,"
               "has_last_ul_over_air_delay,last_ul_over_air_delay_slots,"
               "has_predicted_periodicity,predicted_interarrival_slots,predicted_periodicity_subframes,"
               "scenario\n",
               file);
    std::fflush(file);
    std::fprintf(stderr,
                 "[bsr_ml_dataset] BSR dataset logging enabled -> %s (scenario=%s, periodicBSR-Timer=%u sf, "
                 "retxBSR-Timer=%u sf)\n",
                 path,
                 scenario.c_str(),
                 periodic_bsr_timer_subframes,
                 retx_bsr_timer_subframes);
  }

  bool enabled() const { return file != nullptr; }

  void write(const bsr_sample& s_in)
  {
    if (file == nullptr) {
      return;
    }

    bsr_sample s = s_in;

    {
      std::lock_guard<std::mutex> guard(mtx);
      auto                        it = delay_trackers.find(s.ue_index);
      if (it != delay_trackers.end() && it->second.has_last_delay) {
        s.has_last_ul_over_air_delay   = true;
        s.last_ul_over_air_delay_slots = it->second.last_delay_slots;
      } else {
        s.has_last_ul_over_air_delay   = false;
        s.last_ul_over_air_delay_slots = 0;
      }
    }

    std::string line = fmt::format(

        "{},{},{},{},{},"

        "{},{},"

        "{},{},{},{},{},{},"

        "{},{},{},{:.2f},{:.2f},"

        "{:.3f},"

        "{},"

        "{:.3f},{},{},"

        "{},"

        "{},{},{},"

        "{},{},"

        "{},{:.3f},{},"

        "{}\n",
        s.slot, s.sfn, s.subframe, s.slot_in_frame, s.numerology,
        s.rnti, s.ue_index,
        s.bsr_format, s.bsr_total_bytes, s.lcg0_bytes, s.lcg1_bytes, s.lcg2_bytes, s.lcg3_bytes,
        s.wideband_cqi, s.dl_ri, s.ul_ri, s.pusch_snr_db, s.pusch_avg_sinr_db,
        s.dl_brate_kbps,
        s.pending_ul_bytes,
        s.ul_brate_kbps, s.ul_tb_bytes, s.nof_ul_grants,
        s.sr_count,
        static_cast<unsigned>(s.trigger_type), s.has_interarrival ? 1 : 0, s.interarrival_slots,
        s.has_last_ul_over_air_delay ? 1 : 0, s.last_ul_over_air_delay_slots,
        s.has_predicted_periodicity ? 1 : 0, s.predicted_interarrival_slots, s.predicted_periodicity_subframes,
        scenario);

    std::lock_guard<std::mutex> guard(mtx);
    std::fputs(line.c_str(), file);
    if ((++rows_since_flush) >= flush_period) {
      std::fflush(file);
      rows_since_flush = 0;
    }
  }

  void classify(uint16_t          ue_index,
               uint32_t          slot,
               uint8_t           numerology,
               uint8_t           bsr_format,
               const uint32_t    lcg_bytes[4],
               bsr_trigger_type& trigger_type,
               bool&             has_interarrival,
               uint32_t&         interarrival_slots,
               bool&             has_predicted_periodicity,
               double&           predicted_interarrival_slots,
               uint16_t&         predicted_periodicity_subframes)
  {
    trigger_type                    = bsr_trigger_type::unknown;
    has_interarrival                = false;
    interarrival_slots              = 0;
    has_predicted_periodicity       = false;
    predicted_interarrival_slots    = 0.0;
    predicted_periodicity_subframes = 0;
    if (file == nullptr) {
      return;
    }

    const uint64_t slots_per_subframe = uint64_t{1} << numerology;

    std::lock_guard<std::mutex> guard(mtx);
    bsr_trigger_tracker&        trk = bsr_trackers[ue_index];

    const bool is_truncated = (bsr_format == 2  || bsr_format == 3 );

    if (is_truncated) {
      trigger_type = bsr_trigger_type::padding;
    } else {

      bool regular_new_data = false;
      {
        bool any_lcg_has_data_now = false;
        for (unsigned l = 0; l < MAX_NOF_LCGS_TRACKED; ++l) {
          if (lcg_bytes[l] > 0) {
            any_lcg_has_data_now = true;
            break;
          }
        }
        bool all_lcgs_were_empty = true;
        if (trk.has_last_lcg_bytes) {
          for (unsigned l = 0; l < MAX_NOF_LCGS_TRACKED; ++l) {
            if (trk.last_lcg_bytes[l] > 0) {
              all_lcgs_were_empty = false;
              break;
            }
          }
        }
        regular_new_data = any_lcg_has_data_now && all_lcgs_were_empty;
      }

      bool retx_has_data = false;
      for (unsigned l = 0; l < MAX_NOF_LCGS_TRACKED; ++l) {
        if (lcg_bytes[l] > 0) {
          retx_has_data = true;
          break;
        }
      }
      const bool retx_expired = trk.has_retx_reset && retx_bsr_timer_subframes > 0 &&
                                wrap_forward_distance(trk.retx_reset_slot, slot, numerology) >=
                                    retx_bsr_timer_subframes * slots_per_subframe;
      const bool regular_retx = retx_expired && retx_has_data;

      const bool periodic_expired = trk.has_periodic_reset && periodic_bsr_timer_subframes > 0 &&
                                    wrap_forward_distance(trk.periodic_reset_slot, slot, numerology) >=
                                        periodic_bsr_timer_subframes * slots_per_subframe;

      if (regular_new_data || regular_retx) {
        trigger_type = bsr_trigger_type::regular;
      } else if (periodic_expired) {
        trigger_type = bsr_trigger_type::periodic;
      } else if (trk.has_last_lcg_bytes || trk.has_periodic_reset || trk.has_retx_reset) {

        trigger_type = bsr_trigger_type::padding;
      } else {

        trigger_type = bsr_trigger_type::unknown;
      }
    }

    if (trigger_type == bsr_trigger_type::regular) {
      if (trk.has_last_regular_slot) {
        has_interarrival   = true;
        interarrival_slots = wrap_forward_distance(trk.last_regular_slot, slot, numerology);
      }
      trk.last_regular_slot     = slot;
      trk.has_last_regular_slot = true;

      if (has_interarrival) {
        bsr_ml::predictor& pred = bsr_ml::predictor::instance();
        if (pred.enabled()) {
          const unsigned wsize = pred.window_size();
          if (wsize > 0) {
            trk.interarrival_window.push_back(static_cast<double>(interarrival_slots));
            while (trk.interarrival_window.size() > wsize) {
              trk.interarrival_window.pop_front();
            }
            if (trk.interarrival_window.size() == wsize) {
              const std::vector<double> window(trk.interarrival_window.begin(), trk.interarrival_window.end());
              const bsr_ml::predictor::prediction_result res = pred.predict(window, numerology);
              if (res.has_prediction) {
                has_predicted_periodicity       = true;
                predicted_interarrival_slots    = res.predicted_interarrival_slots;
                predicted_periodicity_subframes = static_cast<uint16_t>(
                    periodic_bsr_timer_to_value(res.mapped_periodicity));

                trk.last_recommended_periodic_bsr_timer = predicted_periodicity_subframes;
              }
            }
          }
        }
      }
    }

    if (!is_truncated) {
      trk.periodic_reset_slot = slot;
      trk.has_periodic_reset  = true;
    }
    trk.retx_reset_slot = slot;
    trk.has_retx_reset  = true;

    for (unsigned l = 0; l < MAX_NOF_LCGS_TRACKED; ++l) {
      trk.last_lcg_bytes[l] = lcg_bytes[l];
    }
    trk.has_last_lcg_bytes = true;
  }

  void record_grant(uint16_t ue_index, uint8_t harq_id, uint32_t slot, bool new_data)
  {
    if (file == nullptr || harq_id >= MAX_NOF_UL_HARQS) {
      return;
    }
    std::lock_guard<std::mutex> guard(mtx);

    ul_delay_tracker& dtrk = delay_trackers[ue_index];
    if (new_data || !dtrk.has_pending_sched[harq_id]) {
      dtrk.pending_sched_slot[harq_id] = slot;
      dtrk.has_pending_sched[harq_id]  = true;
    }

    if (new_data) {
      bsr_trigger_tracker& btrk = bsr_trackers[ue_index];
      btrk.retx_reset_slot      = slot;
      btrk.has_retx_reset       = true;
    }
  }

  void record_crc(uint16_t ue_index, uint8_t harq_id, uint32_t slot, uint8_t numerology)
  {
    if (file == nullptr || harq_id >= MAX_NOF_UL_HARQS) {
      return;
    }
    std::lock_guard<std::mutex> guard(mtx);
    auto                        it = delay_trackers.find(ue_index);
    if (it == delay_trackers.end() || !it->second.has_pending_sched[harq_id]) {

      return;
    }
    ul_delay_tracker& dtrk = it->second;

    const uint32_t delay_slots           = wrap_forward_distance(dtrk.pending_sched_slot[harq_id], slot, numerology);
    dtrk.has_pending_sched[harq_id]      = false;
    dtrk.last_delay_slots                = delay_slots;
    dtrk.has_last_delay                  = true;
  }

  void remove_ue(uint16_t ue_index)
  {
    std::lock_guard<std::mutex> guard(mtx);
    bsr_trackers.erase(ue_index);
    delay_trackers.erase(ue_index);
  }

  unsigned get_recommended_periodic_bsr_timer(uint16_t ue_index)
  {
    std::lock_guard<std::mutex> guard(mtx);
    auto                        it = bsr_trackers.find(ue_index);
    return (it != bsr_trackers.end()) ? it->second.last_recommended_periodic_bsr_timer : 0u;
  }

private:
  static std::string timestamp_now()
  {
    std::time_t t   = std::time(nullptr);
    std::tm     tmv = {};
    ::localtime_r(&t, &tmv);
    char buf[32];
    std::strftime(buf, sizeof(buf), "%Y%m%d_%H%M%S", &tmv);
    return std::string(buf);
  }

  bsr_dataset_writer() = default;

  ~bsr_dataset_writer()
  {
    if (file != nullptr) {
      std::fflush(file);
      std::fclose(file);
    }
  }

  std::FILE*                file             = nullptr;
  std::mutex                mtx;
  unsigned                  rows_since_flush = 0;
  std::string               scenario         = "default";
  bool                      configured       = false;
  static constexpr unsigned flush_period     = 64;

  unsigned periodic_bsr_timer_subframes = 10;
  unsigned retx_bsr_timer_subframes     = 80;

  std::unordered_map<uint16_t, bsr_trigger_tracker> bsr_trackers;
  std::unordered_map<uint16_t, ul_delay_tracker>    delay_trackers;
};

}

void ocudu::bsr_ml_dataset::configure(bool               enabled,
                                      const std::string& output_dir,
                                      const std::string& scenario,
                                      unsigned            periodic_bsr_timer_subframes,
                                      unsigned            retx_bsr_timer_subframes)
{
  bsr_dataset_writer::instance().configure(
      enabled, output_dir, scenario, periodic_bsr_timer_subframes, retx_bsr_timer_subframes);
}

bool ocudu::bsr_ml_dataset::is_enabled()
{
  return bsr_dataset_writer::instance().enabled();
}

void ocudu::bsr_ml_dataset::log_bsr_sample(const bsr_sample& s)
{
  bsr_dataset_writer::instance().write(s);
}

void ocudu::bsr_ml_dataset::classify_bsr_trigger(uint16_t          ue_index,
                                                 uint32_t          slot,
                                                 uint8_t           numerology,
                                                 uint8_t           bsr_format,
                                                 const uint32_t    lcg_bytes[4],
                                                 bsr_trigger_type& trigger_type,
                                                 bool&             has_interarrival,
                                                 uint32_t&         interarrival_slots,
                                                 bool&             has_predicted_periodicity,
                                                 double&           predicted_interarrival_slots,
                                                 uint16_t&         predicted_periodicity_subframes)
{
  bsr_dataset_writer::instance().classify(ue_index,
                                          slot,
                                          numerology,
                                          bsr_format,
                                          lcg_bytes,
                                          trigger_type,
                                          has_interarrival,
                                          interarrival_slots,
                                          has_predicted_periodicity,
                                          predicted_interarrival_slots,
                                          predicted_periodicity_subframes);
}

void ocudu::bsr_ml_dataset::record_ul_grant(uint16_t ue_index, uint8_t harq_id, uint32_t slot, bool new_data)
{
  bsr_dataset_writer::instance().record_grant(ue_index, harq_id, slot, new_data);
}

void ocudu::bsr_ml_dataset::record_ul_crc_success(uint16_t ue_index, uint8_t harq_id, uint32_t slot, uint8_t numerology)
{
  bsr_dataset_writer::instance().record_crc(ue_index, harq_id, slot, numerology);
}

void ocudu::bsr_ml_dataset::remove_ue(uint16_t ue_index)
{
  bsr_dataset_writer::instance().remove_ue(ue_index);
}

unsigned ocudu::bsr_ml_dataset::get_recommended_periodic_bsr_timer(uint16_t ue_index)
{
  return bsr_dataset_writer::instance().get_recommended_periodic_bsr_timer(ue_index);
}
