#include "prach_detector_inline_impl.h"
#include "prach_detector_inline_kernels.h"

#include "../../phy/upper/channel_processors/prach/prach_detector_generic_thresholds.h"

#include "ocudu/phy/upper/channel_processors/prach/factories.h"
#include "ocudu/phy/upper/channel_processors/prach/prach_detector_phy_validator.h"
#include "ocudu/ran/prach/prach_cyclic_shifts.h"
#include "ocudu/ran/prach/prach_preamble_information.h"
#include "ocudu/support/math/math_utils.h"
#include "fmt/format.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <cstdio>

#include <cuda_runtime.h>
#include <cufft.h>

namespace ocudu {
namespace hal {
namespace cuda {

namespace {

constexpr unsigned long long LOG_STATS_EVERY = 1000;

bool prach_bench_logs()
{
  static const bool on = std::getenv("OCUDU_PRACH_BENCH") != nullptr;
  return on;
}

inline bool cuda_ok(cudaError_t e, const char* what)
{
  if (e == cudaSuccess) {
    return true;
  }
  std::fprintf(stderr, "[prach_detector_inline] %s failed: %s\n", what, cudaGetErrorString(e));
  return false;
}

inline bool cufft_ok(cufftResult r, const char* what)
{
  if (r == CUFFT_SUCCESS) {
    return true;
  }
  std::fprintf(stderr, "[prach_detector_inline] %s failed: cufft rc=%d\n", what, static_cast<int>(r));
  return false;
}

void record_inline_detect_stats(unsigned long long&                  total,
                                unsigned long long&                  sum_ns,
                                unsigned long long&                  max_ns,
                                unsigned long long&                  min_ns,
                                std::chrono::steady_clock::time_point t0)
{
  auto ns = static_cast<unsigned long long>(
      std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - t0).count());
  ++total;
  sum_ns += ns;
  if (ns > max_ns) {
    max_ns = ns;
  }
  if (min_ns == 0 || ns < min_ns) {
    min_ns = ns;
  }
  if (prach_bench_logs() && (total % LOG_STATS_EVERY) == 0) {
    fmt::print(stderr,
               "[prach_detector_inline] stats: detects={} mean={}us min={}us max={}us (last_window={})\n",
               total,
               (sum_ns / LOG_STATS_EVERY) / 1000,
               min_ns / 1000,
               max_ns / 1000,
               LOG_STATS_EVERY);
    sum_ns = 0;
    max_ns = 0;
    min_ns = 0;
  }
}

inline cufftHandle    as_plan(int  stored)       { return static_cast<cufftHandle>(stored); }
inline int            store_plan(cufftHandle h)  { return static_cast<int>(h); }

template <typename T>
bool grow_device(void*& ptr, std::size_t want_bytes)
{
  if (ptr != nullptr) {
    cudaFree(ptr);
    ptr = nullptr;
  }
  return cuda_ok(cudaMalloc(&ptr, want_bytes), "cudaMalloc");
}

template <typename T>
bool grow_pinned(void*& ptr, std::size_t want_bytes)
{
  if (ptr != nullptr) {
    cudaFreeHost(ptr);
    ptr = nullptr;
  }
  return cuda_ok(cudaMallocHost(&ptr, want_bytes), "cudaMallocHost");
}

}

std::unique_ptr<prach_detector_inline_impl> prach_detector_inline_impl::create(const prach_detector_inline_config& cfg)
{

  int n_dev = 0;
  if (cudaGetDeviceCount(&n_dev) != cudaSuccess || n_dev == 0) {
    std::fprintf(stderr, "[prach_detector_inline] no CUDA device available\n");
    return nullptr;
  }
  std::unique_ptr<prach_detector_inline_impl> p{new prach_detector_inline_impl()};
  p->cfg = cfg;

  cudaStream_t s = nullptr;
  if (!cuda_ok(cudaStreamCreateWithFlags(&s, cudaStreamNonBlocking), "cudaStreamCreate")) {
    return nullptr;
  }
  p->stream = s;

  auto gen_factory = create_prach_generator_factory_sw();
  if (!gen_factory) {
    std::fprintf(stderr, "[prach_detector_inline] prach_generator_factory_sw returned null\n");
    return nullptr;
  }
  p->generator = gen_factory->create();

  fmt::print(stderr,
             "[prach_detector_inline] constructed: idft_long={} idft_short={} max_batch={}\n",
             cfg.idft_long_size,
             cfg.idft_short_size,
             cfg.max_batch);
  return p;
}

prach_detector_inline_impl::~prach_detector_inline_impl()
{
  if (plan_long != 0) {
    cufftDestroy(as_plan(plan_long));
  }
  if (plan_short != 0) {
    cufftDestroy(as_plan(plan_short));
  }
  cudaFree(d_combined);
  cudaFree(d_no_root);
  cudaFree(d_idft_in);
  cudaFree(d_idft_out);
  cudaFree(d_mod_sq);
  cudaFree(d_num);
  cudaFree(d_den);
  cudaFree(d_delay);
  cudaFree(d_detected);
  cudaFree(d_metric);
  cudaFree(d_power);
  cudaFree(d_root);
  cudaFree(d_rssi_per_tuple);
  cudaFreeHost(h_delay);
  cudaFreeHost(h_detected);
  cudaFreeHost(h_metric);
  cudaFreeHost(h_power);
  cudaFreeHost(h_rssi_per_tuple);
  cudaFreeHost(h_root);
  if (stream != nullptr) {
    cudaStreamDestroy(static_cast<cudaStream_t>(stream));
  }
}

bool prach_detector_inline_impl::ensure_resources(bool      long_pre,
                                                  unsigned  nof_sequences,
                                                  unsigned  batch,
                                                  unsigned  dft_size,
                                                  unsigned  nof_shifts,
                                                  unsigned  win_width)
{
  const std::size_t total_batch = static_cast<std::size_t>(nof_sequences) * batch;

  const std::size_t max_rssi_tuples = std::max<std::size_t>(alloc_max_rssi_tuples, batch * 16u);
  if (max_rssi_tuples > alloc_max_rssi_tuples) {
    if (!grow_device<float>(d_rssi_per_tuple, max_rssi_tuples * sizeof(float))) {
      return false;
    }
    if (!grow_pinned<float>(h_rssi_per_tuple, max_rssi_tuples * sizeof(float))) {
      return false;
    }
    alloc_max_rssi_tuples = max_rssi_tuples;
  }

  const std::size_t max_L_ra_now    = long_pre ? 839u : 139u;
  const std::size_t max_L_ra        = std::max<std::size_t>(alloc_max_L_ra, max_L_ra_now);
  const std::size_t max_dft         = std::max<std::size_t>(alloc_max_dft, dft_size);
  const std::size_t max_batch       = std::max<std::size_t>(alloc_max_batch, batch);
  const std::size_t max_total_batch = std::max<std::size_t>(alloc_max_total_batch, total_batch);

  const bool need_resize_samples = (max_L_ra != alloc_max_L_ra) || (max_dft != alloc_max_dft) ||
                                   (max_batch != alloc_max_batch) ||
                                   (max_total_batch != alloc_max_total_batch);
  if (need_resize_samples) {
    const std::size_t cf_combined = max_batch * max_L_ra * 2u * sizeof(float);
    const std::size_t cf_lr       = max_total_batch * max_L_ra * 2u * sizeof(float);
    const std::size_t cf_d        = max_total_batch * max_dft * 2u * sizeof(float);
    const std::size_t f_d         = max_total_batch * max_dft * sizeof(float);
    if (!grow_device<float>(d_combined, cf_combined) || !grow_device<float>(d_no_root, cf_lr) ||
        !grow_device<float>(d_idft_in, cf_d) || !grow_device<float>(d_idft_out, cf_d) ||
        !grow_device<float>(d_mod_sq, f_d)) {
      return false;
    }
    alloc_max_L_ra        = max_L_ra;
    alloc_max_dft         = max_dft;
    alloc_max_batch       = max_batch;
    alloc_max_total_batch = max_total_batch;
  }

  const std::size_t seq_shift     = static_cast<std::size_t>(nof_sequences) * nof_shifts;
  const std::size_t seq_shift_win = seq_shift * win_width;
  const std::size_t max_ss        = std::max<std::size_t>(alloc_max_seq_shift, seq_shift);
  const std::size_t max_ssw       = std::max<std::size_t>(alloc_max_seq_shift_win, seq_shift_win);
  const bool need_resize_shifts   = (max_ss != alloc_max_seq_shift) || (max_ssw != alloc_max_seq_shift_win);
  if (need_resize_shifts) {
    if (!grow_device<float>(d_num, max_ssw * sizeof(float)) ||
        !grow_device<float>(d_den, max_ssw * sizeof(float)) ||
        !grow_device<uint32_t>(d_delay, max_ss * sizeof(uint32_t)) ||
        !grow_device<unsigned char>(d_detected, max_ss * sizeof(unsigned char)) ||
        !grow_device<float>(d_metric, max_ss * sizeof(float)) ||
        !grow_device<float>(d_power, max_ss * sizeof(float))) {
      return false;
    }
    if (!grow_pinned<uint32_t>(h_delay, max_ss * sizeof(uint32_t)) ||
        !grow_pinned<unsigned char>(h_detected, max_ss * sizeof(unsigned char)) ||
        !grow_pinned<float>(h_metric, max_ss * sizeof(float)) ||
        !grow_pinned<float>(h_power, max_ss * sizeof(float))) {
      return false;
    }
    alloc_max_seq_shift     = max_ss;
    alloc_max_seq_shift_win = max_ssw;
  }

  if (d_root == nullptr) {
    const std::size_t root_bytes = static_cast<std::size_t>(64) * alloc_max_L_ra * 2u * sizeof(float);
    if (!grow_device<float>(d_root, root_bytes) || !grow_pinned<float>(h_root, root_bytes)) {
      return false;
    }
  }

  int&      plan_stored = long_pre ? plan_long : plan_short;
  unsigned& plan_batch  = long_pre ? plan_long_batch : plan_short_batch;
  if (plan_stored == 0 || plan_batch != total_batch) {
    if (plan_stored != 0) {
      cufftDestroy(as_plan(plan_stored));
      plan_stored = 0;
    }
    cufftHandle plan_handle = 0;
    int         n           = static_cast<int>(dft_size);
    if (!cufft_ok(cufftPlanMany(&plan_handle,
                                 1,
                                 &n,
                                 nullptr,
                                 1,
                                 n,
                                 nullptr,
                                 1,
                                 n,
                                 CUFFT_C2C,
                                 static_cast<int>(total_batch)),
                  "cufftPlanMany")) {
      return false;
    }
    if (!cufft_ok(cufftSetStream(plan_handle, static_cast<cudaStream_t>(stream)), "cufftSetStream")) {
      return false;
    }
    plan_stored = store_plan(plan_handle);
    plan_batch  = static_cast<unsigned>(total_batch);
  }
  return true;
}

void prach_detector_inline_impl::update_root_cache(const prach_detector::configuration& config,
                                                   unsigned                             nof_sequences,
                                                   unsigned                             nof_shifts,
                                                   unsigned                             L_ra)
{
  bool dirty = !roots_cached || cached_format != config.format || cached_root_idx != config.root_sequence_index ||
               cached_restricted != config.restricted_set || cached_zcz != config.zero_correlation_zone ||
               cached_nof_sequences != nof_sequences || cached_nof_shifts != nof_shifts || cached_L_ra != L_ra;
  if (!dirty) {
    return;
  }

  auto* h_root_cf = static_cast<float*>(h_root);
  for (unsigned i_seq = 0; i_seq < nof_sequences; ++i_seq) {
    prach_generator::configuration gen{};
    gen.format                = config.format;
    gen.root_sequence_index   = config.root_sequence_index;
    gen.preamble_index        = i_seq * nof_shifts;
    gen.restricted_set        = config.restricted_set;
    gen.zero_correlation_zone = config.zero_correlation_zone;
    span<const cf_t> root     = generator->generate(gen);
    std::memcpy(h_root_cf + i_seq * L_ra * 2u, root.data(), L_ra * sizeof(cf_t));
  }
  cudaMemcpyAsync(d_root,
                  h_root_cf,
                  static_cast<std::size_t>(nof_sequences) * L_ra * 2u * sizeof(float),
                  cudaMemcpyHostToDevice,
                  static_cast<cudaStream_t>(stream));
  cached_format        = config.format;
  cached_root_idx      = config.root_sequence_index;
  cached_restricted    = config.restricted_set;
  cached_zcz           = config.zero_correlation_zone;
  cached_nof_sequences = nof_sequences;
  cached_nof_shifts    = nof_shifts;
  cached_L_ra          = L_ra;
  roots_cached         = true;
}

prach_detection_result prach_detector_inline_impl::detect_inline(const vram_prach_buffer&             input,
                                                                  const prach_detector::configuration& config)
{
  auto t0 = std::chrono::steady_clock::now();

  prach_preamble_information preamble_info = is_long_preamble(config.format)
                                                 ? get_prach_preamble_long_info(config.format)
                                                 : get_prach_preamble_short_info(config.format, config.ra_scs, false);
  interval<unsigned> preamble_indices(config.start_preamble_index,
                                      config.start_preamble_index + config.nof_preamble_indices);
  unsigned N_cs = prach_cyclic_shifts_get(config.ra_scs, config.restricted_set, config.zero_correlation_zone);
  unsigned L_ra = is_long_preamble(config.format) ? prach_constants::LONG_SEQUENCE_LENGTH
                                                  : prach_constants::SHORT_SEQUENCE_LENGTH;
  unsigned nof_shifts    = 1;
  unsigned nof_sequences = 64;
  if (N_cs != 0) {
    nof_shifts    = std::min(prach_constants::MAX_NUM_PREAMBLES, L_ra / N_cs);
    nof_sequences = divide_ceil(64u, nof_shifts);
  }
  const bool long_pre = is_long_preamble(config.format);
  unsigned   dft_size = long_pre ? cfg.idft_long_size : cfg.idft_short_size;

  double sampling_rate_Hz = static_cast<double>(dft_size) * ra_scs_to_Hz(preamble_info.scs);
  double cp_duration      = preamble_info.cp_length.to_seconds();
  auto   cp_prach =
      static_cast<unsigned>(std::floor(cp_duration * static_cast<double>(L_ra) * ra_scs_to_Hz(preamble_info.scs)));

  unsigned win_width = std::min(N_cs, cp_prach);
  if (N_cs == 0) {
    win_width = cp_prach;
  }
  if (win_width == L_ra) {
    win_width -= 20;
  }
  win_width = (win_width * dft_size) / L_ra;

  detail::threshold_params th_params;
  th_params.nof_rx_ports          = config.nof_rx_ports;
  th_params.scs                   = config.ra_scs;
  th_params.format                = config.format;
  th_params.zero_correlation_zone = config.zero_correlation_zone;
  auto [threshold, combine_symbols, win_margin] = detail::get_threshold_and_margin(th_params);

  unsigned max_delay_samples = (N_cs == 0) ? cp_prach : std::min(std::max(N_cs, 1U) - 1U, cp_prach);
  max_delay_samples          = (max_delay_samples * dft_size) / L_ra;
  unsigned nof_symbols       = preamble_info.nof_symbols;
  const unsigned batch       = combine_symbols ? config.nof_rx_ports : (config.nof_rx_ports * nof_symbols);

  prach_detection_result result;
  result.preambles.clear();
  result.time_resolution  = phy_time_unit::from_seconds(1.0 / sampling_rate_Hz);
  result.time_advance_max = phy_time_unit::from_seconds(static_cast<double>(max_delay_samples) * 0.8 / sampling_rate_Hz);

  if (!ensure_resources(long_pre, nof_sequences, batch, dft_size, nof_shifts, win_width)) {

    record_inline_detect_stats(total_detects, sum_detect_ns, max_detect_ns, min_detect_ns, t0);
    return result;
  }
  cudaStream_t   s           = static_cast<cudaStream_t>(stream);
  const unsigned S           = nof_sequences;
  const unsigned total_batch = S * batch;

  const auto&    vc         = input.get_config();
  const unsigned sym_stride = vc.sequence_length;
  const unsigned ps_stride  = vc.nof_symbols * vc.sequence_length;

  unsigned power_normalization = config.nof_rx_ports * L_ra * nof_symbols;
  if (combine_symbols) {
    power_normalization *= nof_symbols;
  }

  update_root_cache(config, nof_sequences, nof_shifts, L_ra);

  const float scale_mod = 1.0F / static_cast<float>(dft_size * L_ra);

  launch_rssi_reduce(static_cast<float*>(d_rssi_per_tuple),
                     input.device_base(),
                     config.nof_rx_ports,
                     nof_symbols,
                     L_ra,
                     sym_stride,
                     ps_stride,
                     s);

  cudaMemsetAsync(d_num, 0, static_cast<std::size_t>(S) * nof_shifts * win_width * sizeof(float), s);
  cudaMemsetAsync(d_den, 0, static_cast<std::size_t>(S) * nof_shifts * win_width * sizeof(float), s);

  launch_cbf16_to_cf_combine(d_combined,
                             input.device_base(),
                             config.nof_rx_ports,
                             nof_symbols,
                             L_ra,
                             sym_stride,
                             ps_stride,
                             combine_symbols,
                             s);

  launch_prod_conj(d_no_root, d_combined, d_root, batch, L_ra, S, s);

  launch_bin_reorder(d_idft_in, d_no_root, total_batch, L_ra, dft_size, s);

  cufftHandle plan = as_plan(long_pre ? plan_long : plan_short);
  cufftExecC2C(plan,
               static_cast<cufftComplex*>(d_idft_in),
               static_cast<cufftComplex*>(d_idft_out),
               CUFFT_INVERSE);

  launch_modulus_square_scale(static_cast<float*>(d_mod_sq), d_idft_out, total_batch, dft_size, scale_mod, s);

  launch_per_shift_accumulate(static_cast<float*>(d_num),
                              static_cast<float*>(d_den),
                              static_cast<const float*>(d_mod_sq),
                              batch,
                              dft_size,
                              L_ra,
                              N_cs,
                              nof_shifts,
                              win_width,
                              win_margin,
                              S,
                              s);

  launch_finalize_argmax(static_cast<uint32_t*>(d_delay),
                         static_cast<unsigned char*>(d_detected),
                         static_cast<float*>(d_metric),
                         static_cast<float*>(d_power),
                         static_cast<float*>(d_num),
                         static_cast<float*>(d_den),
                         nof_shifts,
                         win_width,
                         S,
                         threshold,
                         config.detection_threshold_margin,
                         max_delay_samples,
                         power_normalization,
                         s);

  const std::size_t seq_shift = static_cast<std::size_t>(S) * nof_shifts;
  cudaMemcpyAsync(h_rssi_per_tuple,
                  d_rssi_per_tuple,
                  config.nof_rx_ports * nof_symbols * sizeof(float),
                  cudaMemcpyDeviceToHost,
                  s);
  cudaMemcpyAsync(h_delay, d_delay, seq_shift * sizeof(uint32_t), cudaMemcpyDeviceToHost, s);
  cudaMemcpyAsync(h_detected, d_detected, seq_shift * sizeof(unsigned char), cudaMemcpyDeviceToHost, s);
  cudaMemcpyAsync(h_metric, d_metric, seq_shift * sizeof(float), cudaMemcpyDeviceToHost, s);
  cudaMemcpyAsync(h_power, d_power, seq_shift * sizeof(float), cudaMemcpyDeviceToHost, s);

  cudaStreamSynchronize(s);

  float rssi = 0.0F;
  for (unsigned i = 0; i < config.nof_rx_ports * nof_symbols; ++i) {
    rssi += static_cast<float*>(h_rssi_per_tuple)[i];
  }
  rssi /= static_cast<float>(config.nof_rx_ports * nof_symbols);
  result.rssi_dB = convert_power_to_dB(rssi);
  if (!std::isnormal(rssi)) {
    record_inline_detect_stats(total_detects, sum_detect_ns, max_detect_ns, min_detect_ns, t0);
    return result;
  }

  for (unsigned i_seq = 0; i_seq < S; ++i_seq) {
    interval<unsigned> seq_preambles(i_seq * nof_shifts, (i_seq + 1) * nof_shifts);
    if (!preamble_indices.overlaps(seq_preambles)) {
      continue;
    }
    for (unsigned i_window = 0; i_window < nof_shifts; ++i_window) {
      unsigned preamble_index = i_seq * nof_shifts + i_window;
      if (!preamble_indices.contains(preamble_index)) {
        continue;
      }
      const unsigned flat = i_seq * nof_shifts + i_window;
      if (static_cast<unsigned char*>(h_detected)[flat] == 0) {
        continue;
      }
      uint32_t delay = static_cast<uint32_t*>(h_delay)[flat];
      prach_detection_result::preamble_indication& info = result.preambles.emplace_back();
      info.preamble_index    = preamble_index;
      info.time_advance      = phy_time_unit::from_seconds(static_cast<double>(delay) / sampling_rate_Hz);
      info.detection_metric  = static_cast<float*>(h_metric)[flat];
      info.preamble_power_dB = convert_power_to_dB(static_cast<float*>(h_power)[flat]);
    }
  }

  record_inline_detect_stats(total_detects, sum_detect_ns, max_detect_ns, min_detect_ns, t0);
  return result;
}

std::unique_ptr<prach_detector_inline> create_prach_detector_inline(const prach_detector_inline_config& cfg)
{
  return prach_detector_inline_impl::create(cfg);
}

}
}
}
