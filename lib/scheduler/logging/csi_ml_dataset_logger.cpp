#include "csi_ml_dataset_logger.h"
#include "fmt/format.h"
#include <cmath>
#include <cstdio>
#include <ctime>
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
using namespace ocudu::csi_ml_dataset;

namespace {

constexpr uint64_t NOF_SFNS               = 1024;
constexpr uint64_t NOF_SUBFRAMES_PER_FRAME = 10;

uint32_t wrap_forward_distance(uint32_t from, uint32_t to, uint8_t numerology)
{
  const uint64_t modulus = NOF_SFNS * NOF_SUBFRAMES_PER_FRAME * (uint64_t{1} << numerology);
  const uint64_t f       = static_cast<uint64_t>(from) % modulus;
  const uint64_t t       = static_cast<uint64_t>(to) % modulus;
  return static_cast<uint32_t>((t + modulus - f) % modulus);
}

struct csi_ue_tracker {
  bool     has_last_slot = false;
  uint32_t last_slot     = 0;
};

class csi_dataset_writer
{
public:
  static csi_dataset_writer& instance()
  {
    static csi_dataset_writer inst;
    return inst;
  }

  void configure(bool en, const std::string& output_dir, const std::string& scenario_tag)
  {
    if (configured) {
      return;
    }
    configured = true;
    if (!en) {
      return;
    }
    if (!scenario_tag.empty()) {
      scenario = scenario_tag;
    }
    std::string dir = output_dir.empty() ? std::string("ml/datasets") : output_dir;
    mkdir_p(dir);
    std::string out  = fmt::format("{}/csi_ml_dataset_{}.csv", dir, timestamp_now());
    const char* path = out.c_str();
    file             = std::fopen(path, "w");
    if (file == nullptr) {
      std::fprintf(stderr, "[csi_ml_dataset] Failed to open '%s' for writing. CSI dataset logging disabled.\n", path);
      return;
    }

    std::fputs("slot,sfn,subframe,slot_in_frame,numerology,"
               "rnti,ue_index,"
               "valid,wideband_cqi,dl_ri,dl_cri,"
               "dl_cqi_offset_db,effective_cqi,dl_olla_enabled,"
               "slots_since_last_csi,"
               "csi_report_type,"
               "window_p0,window_p1,window_p2,window_p3,"
               "ue_estimated_doppler_hz,"
               "predicted_effective_cqi_wiener,predicted_effective_cqi_gru,"
               "scenario\n",
               file);
    std::fflush(file);
    std::fprintf(stderr, "[csi_ml_dataset] CSI dataset logging enabled -> %s (scenario=%s)\n", path, scenario.c_str());
  }

  bool enabled() const { return file != nullptr; }

  void write(const csi_sample& s)
  {
    if (file == nullptr) {
      return;
    }

    std::lock_guard<std::mutex> guard(mtx);

    int64_t       slots_since_last_csi = -1;
    csi_ue_tracker& trk                = trackers[s.ue_index];
    if (trk.has_last_slot) {
      slots_since_last_csi = static_cast<int64_t>(wrap_forward_distance(trk.last_slot, s.slot, s.numerology));
    }
    trk.last_slot     = s.slot;
    trk.has_last_slot = true;

    std::string w0 = s.has_window ? fmt::format("{:.4f}", s.window[0]) : std::string();
    std::string w1 = s.has_window ? fmt::format("{:.4f}", s.window[1]) : std::string();
    std::string w2 = s.has_window ? fmt::format("{:.4f}", s.window[2]) : std::string();
    std::string w3 = s.has_window ? fmt::format("{:.4f}", s.window[3]) : std::string();
    std::string pred_wiener = (s.pred_kind == 1) ? fmt::format("{:.4f}", s.pred_value) : std::string();
    std::string pred_gru    = (s.pred_kind == 2) ? fmt::format("{:.4f}", s.pred_value) : std::string();

    std::string line = fmt::format(
        "{},{},{},{},{},"
        "{},{},"
        "{},{},{},{},"
        "{:.4f},{:.4f},{},"
        "{},"
        "unknown,"
        "{},{},{},{},"
        ","
        "{},{},"
        "{}\n",
        s.slot, s.sfn, s.subframe, s.slot_in_frame, s.numerology,
        s.rnti, s.ue_index,
        s.valid, s.wideband_cqi, s.dl_ri, s.dl_cri,
        s.dl_cqi_offset_db, s.effective_cqi, s.dl_olla_enabled,
        slots_since_last_csi,
        w0, w1, w2, w3,
        pred_wiener, pred_gru,
        scenario);

    std::fputs(line.c_str(), file);
    if ((++rows_since_flush) >= flush_period) {
      std::fflush(file);
      rows_since_flush = 0;
    }
  }

  void remove_ue(uint16_t ue_index)
  {
    std::lock_guard<std::mutex> guard(mtx);
    trackers.erase(ue_index);
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

  csi_dataset_writer() = default;

  ~csi_dataset_writer()
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

  std::unordered_map<uint16_t, csi_ue_tracker> trackers;
};

class kpi_dataset_writer
{
public:
  static kpi_dataset_writer& instance()
  {
    static kpi_dataset_writer inst;
    return inst;
  }

  void configure(bool en, const std::string& output_dir, const std::string& scenario_tag)
  {
    if (configured) {
      return;
    }
    configured = true;
    if (!en) {
      return;
    }
    if (!scenario_tag.empty()) {
      scenario = scenario_tag;
    }
    std::string dir = output_dir.empty() ? std::string("ml/datasets") : output_dir;
    mkdir_p(dir);
    std::string out  = fmt::format("{}/csi_ml_kpi_{}.csv", dir, timestamp_now());
    const char* path = out.c_str();
    file             = std::fopen(path, "w");
    if (file == nullptr) {
      std::fprintf(stderr, "[csi_ml_dataset] Failed to open '%s' for writing. KPI logging disabled.\n", path);
      return;
    }

    std::fputs("slot,rnti,ue_index,"
               "dl_brate_kbps,dl_bler_pct,dl_nof_ok,dl_nof_nok,"
               "dl_mcs,mean_cqi,dl_ri,"
               "scenario\n",
               file);
    std::fflush(file);
    std::fprintf(stderr, "[csi_ml_dataset] KPI logging enabled -> %s (scenario=%s)\n", path, scenario.c_str());
  }

  bool enabled() const { return file != nullptr; }

  void write(const kpi_sample& s)
  {
    if (file == nullptr) {
      return;
    }

    std::lock_guard<std::mutex> guard(mtx);

    const uint64_t tot = static_cast<uint64_t>(s.dl_nof_ok) + s.dl_nof_nok;
    std::string    bler =
        tot > 0 ? fmt::format("{:.4f}", 100.0 * static_cast<double>(s.dl_nof_nok) / static_cast<double>(tot))
                : std::string();

    std::string cqi = std::isnan(s.mean_cqi) ? std::string() : fmt::format("{:.3f}", s.mean_cqi);
    std::string ri  = std::isnan(s.dl_ri) ? std::string() : fmt::format("{:.3f}", s.dl_ri);

    std::string line = fmt::format("{},{},{},"
                                   "{:.3f},{},{},{},"
                                   "{},{},{},"
                                   "{}\n",
                                   s.slot, s.rnti, s.ue_index,
                                   s.dl_brate_kbps, bler, s.dl_nof_ok, s.dl_nof_nok,
                                   static_cast<unsigned>(s.dl_mcs), cqi, ri,
                                   scenario);

    std::fputs(line.c_str(), file);
    if ((++rows_since_flush) >= flush_period) {
      std::fflush(file);
      rows_since_flush = 0;
    }
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

  kpi_dataset_writer() = default;

  ~kpi_dataset_writer()
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
};

}

void ocudu::csi_ml_dataset::configure(bool enabled, const std::string& output_dir, const std::string& scenario)
{
  csi_dataset_writer::instance().configure(enabled, output_dir, scenario);
  kpi_dataset_writer::instance().configure(enabled, output_dir, scenario);
}

bool ocudu::csi_ml_dataset::is_enabled()
{
  return csi_dataset_writer::instance().enabled();
}

void ocudu::csi_ml_dataset::log_csi_sample(const csi_sample& s)
{
  csi_dataset_writer::instance().write(s);
}

void ocudu::csi_ml_dataset::remove_ue(uint16_t ue_index)
{
  csi_dataset_writer::instance().remove_ue(ue_index);
}

void ocudu::csi_ml_dataset::log_kpi_sample(const kpi_sample& s)
{
  kpi_dataset_writer::instance().write(s);
}

bool ocudu::csi_ml_dataset::is_kpi_enabled()
{
  return kpi_dataset_writer::instance().enabled();
}
