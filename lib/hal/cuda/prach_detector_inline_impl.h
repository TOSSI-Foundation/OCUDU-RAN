#pragma once

#include "ocudu/hal/cuda/prach_detector_inline.h"
#include "ocudu/phy/upper/channel_processors/prach/prach_detector.h"
#include "ocudu/phy/upper/channel_processors/prach/prach_generator.h"
#include "ocudu/ran/prach/prach_format_type.h"
#include "ocudu/ran/prach/restricted_set_config.h"

#include <cstdint>
#include <cstdlib>
#include <memory>
#include <vector>

namespace ocudu {
namespace hal {
namespace cuda {

class prach_detector_inline_impl : public prach_detector_inline
{
public:
  static std::unique_ptr<prach_detector_inline_impl> create(const prach_detector_inline_config& cfg);

  ~prach_detector_inline_impl() override;

  prach_detection_result detect_inline(const vram_prach_buffer&             input,
                                       const prach_detector::configuration& config) override;

private:
  prach_detector_inline_impl() = default;

  bool ensure_resources(bool long_pre, unsigned nof_sequences, unsigned batch, unsigned dft_size,
                        unsigned nof_shifts, unsigned win_width);

  void update_root_cache(const prach_detector::configuration& config, unsigned nof_sequences,
                         unsigned nof_shifts, unsigned L_ra);

  prach_detector_inline_config cfg{};

  void* stream     = nullptr;

  int  plan_long  = 0;
  unsigned plan_long_batch = 0;
  int  plan_short = 0;
  unsigned plan_short_batch = 0;

  void* d_combined        = nullptr;
  void* d_no_root         = nullptr;
  void* d_idft_in         = nullptr;
  void* d_idft_out        = nullptr;
  void* d_mod_sq          = nullptr;
  void* d_num             = nullptr;
  void* d_den             = nullptr;
  void* d_delay           = nullptr;
  void* d_detected        = nullptr;
  void* d_metric          = nullptr;
  void* d_power           = nullptr;
  void* d_root            = nullptr;
  void* d_rssi_per_tuple  = nullptr;

  void* h_delay           = nullptr;
  void* h_detected        = nullptr;
  void* h_metric          = nullptr;
  void* h_power           = nullptr;
  void* h_rssi_per_tuple  = nullptr;
  void* h_root            = nullptr;

  std::size_t alloc_max_batch         = 0;
  std::size_t alloc_max_total_batch   = 0;
  std::size_t alloc_max_L_ra          = 0;
  std::size_t alloc_max_dft           = 0;
  std::size_t alloc_max_seq_shift     = 0;
  std::size_t alloc_max_seq_shift_win = 0;
  std::size_t alloc_max_rssi_tuples   = 0;

  bool                  roots_cached         = false;
  prach_format_type     cached_format        = prach_format_type::zero;
  unsigned              cached_root_idx      = 0;
  restricted_set_config cached_restricted    = restricted_set_config::UNRESTRICTED;
  unsigned              cached_zcz           = 0;
  unsigned              cached_nof_sequences = 0;
  unsigned              cached_nof_shifts    = 0;
  unsigned              cached_L_ra          = 0;

  std::unique_ptr<prach_generator> generator;

  unsigned long long total_detects  = 0;
  unsigned long long sum_detect_ns  = 0;
  unsigned long long max_detect_ns  = 0;
  unsigned long long min_detect_ns  = 0;
};

}
}
}
