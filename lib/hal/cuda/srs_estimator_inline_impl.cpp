#include "ocudu/hal/cuda/srs_estimator_inline.h"

#include "srs_inline_estimator_kernels.h"

#include "ocudu/phy/upper/sequence_generators/low_papr_sequence_generator.h"
#include "ocudu/phy/upper/sequence_generators/sequence_generator_factories.h"
#include "ocudu/ran/resource_block.h"
#include "ocudu/ran/srs/srs_information.h"
#include "ocudu/ran/phy_time_unit.h"
#include "ocudu/support/math/math_utils.h"
#include <cmath>
#include <complex>
#include <cstdio>
#include <cstring>
#include <cufft.h>
#include <cuda_runtime.h>
#include <limits>
#include <new>
#include <utility>
#include <vector>

namespace ocudu {
namespace hal {
namespace cuda {

namespace {

constexpr size_t CF_BYTES = sizeof(float) * 2;

class srs_estimator_inline_impl : public srs_estimator_inline
{
public:
  explicit srs_estimator_inline_impl(const config& cfg) : cfg_(cfg) {}

  ~srs_estimator_inline_impl() override
  {
    if (graph_exec_) { cudaGraphExecDestroy(graph_exec_); }
    if (graph_)      { cudaGraphDestroy(graph_); }
    if (batch_plan_) { cufftDestroy(batch_plan_); }
    free_scratch();
    if (stream_) {
      cudaStreamDestroy(stream_);
    }
  }

  bool init()
  {
    max_batch_ = std::max(1u, cfg_.max_batch);

    if (cudaError_t e = cudaStreamCreate(&stream_); e != cudaSuccess) {
      std::fprintf(stderr, "[srs_estimator_inline] cudaStreamCreate failed: %s\n", cudaGetErrorString(e));
      return false;
    }

    max_dft_size_ = pow2(log2_ceil(static_cast<unsigned>(MAX_NOF_SUBCARRIERS)));
    min_dft_size_ = pow2(log2_ceil(static_cast<unsigned>(
        1.0F / (15000 * phy_time_unit::from_timing_advance(1, subcarrier_spacing::kHz15).to_seconds()))));

    const size_t B = max_batch_;
    if (cudaMalloc(&d_temp_lse_, B * max_lse_elems() * CF_BYTES) != cudaSuccess) return alloc_fail("temp_lse");
    if (cudaMalloc(&d_temp_noise_, B * max_noise_elems() * CF_BYTES) != cudaSuccess) return alloc_fail("temp_noise");
    if (cudaMalloc(&d_zc_seq_, B * max_zc_elems() * CF_BYTES) != cudaSuccess) return alloc_fail("zc_seq");
    if (cudaMalloc(&d_channel_coeffs_, B * max_coeff_elems() * CF_BYTES) != cudaSuccess) return alloc_fail("channel_coeffs");
    if (cudaMalloc(reinterpret_cast<void**>(&d_epre_), B * max_epre_elems() * sizeof(float)) != cudaSuccess) return alloc_fail("epre");
    if (cudaMalloc(reinterpret_cast<void**>(&d_nsq_), B * max_epre_elems() * sizeof(float)) != cudaSuccess) return alloc_fail("nsq");

    const size_t idft_bytes = B * static_cast<size_t>(cfg_.max_nof_rx_ports) * max_dft_size_ * CF_BYTES;
    if (cudaMalloc(&d_idft_in_, idft_bytes) != cudaSuccess) return alloc_fail("idft_in");
    if (cudaMalloc(&d_idft_out_, idft_bytes) != cudaSuccess) return alloc_fail("idft_out");
    if (cudaMalloc(reinterpret_cast<void**>(&d_correlation_), B * max_dft_size_ * sizeof(float)) != cudaSuccess) return alloc_fail("correlation");
    if (cudaMalloc(reinterpret_cast<void**>(&d_phase_per_tx_), B * cfg_.max_nof_antenna_ports * sizeof(float)) != cudaSuccess) return alloc_fail("phase_per_tx");
    if (cudaMalloc(reinterpret_cast<void**>(&d_phase_per_group_), B * 2 * sizeof(float)) != cudaSuccess) return alloc_fail("phase_per_group");
    if (cudaMalloc(reinterpret_cast<void**>(&d_phase_shift_subcarrier_), B * sizeof(float)) != cudaSuccess) return alloc_fail("phase_sc");
    if (cudaMalloc(reinterpret_cast<void**>(&d_time_alignment_), B * sizeof(float)) != cudaSuccess) return alloc_fail("time_alignment");
    if (cudaMalloc(reinterpret_cast<void**>(&d_desc_), B * sizeof(srs_occ_desc)) != cudaSuccess) return alloc_fail("desc");

    if (cudaMallocHost(&h_zc_seq_, B * max_zc_elems() * CF_BYTES) != cudaSuccess) return alloc_fail("h_zc");
    if (cudaMallocHost(&h_channel_coeffs_, B * max_coeff_elems() * CF_BYTES) != cudaSuccess) return alloc_fail("h_coeffs");
    if (cudaMallocHost(reinterpret_cast<void**>(&h_epre_), B * max_epre_elems() * sizeof(float)) != cudaSuccess) return alloc_fail("h_epre");
    if (cudaMallocHost(reinterpret_cast<void**>(&h_nsq_), B * max_epre_elems() * sizeof(float)) != cudaSuccess) return alloc_fail("h_nsq");
    if (cudaMallocHost(reinterpret_cast<void**>(&h_time_alignment_), B * sizeof(float)) != cudaSuccess) return alloc_fail("h_ta");
    if (cudaMallocHost(reinterpret_cast<void**>(&h_desc_), B * sizeof(srs_occ_desc)) != cudaSuccess) return alloc_fail("h_desc");

    auto lpg_factory = create_low_papr_sequence_generator_sw_factory();
    if (!lpg_factory) {
      std::fprintf(stderr, "[srs_estimator_inline] lpg factory creation failed\n");
      return false;
    }
    lpg_ = lpg_factory->create();
    if (!lpg_) {
      std::fprintf(stderr, "[srs_estimator_inline] lpg create failed\n");
      return false;
    }

    valid_.resize(max_batch_, 0);
    return true;
  }

  srs_estimator_result estimate_inline(const vram_srs_buffer&             vram_buf,
                                       const srs_estimator_configuration& sr_cfg) override
  {
    srs_estimator_result result;
    const vram_srs_buffer* bufp = &vram_buf;
    if (!prepare_batch(&bufp, &sr_cfg, 1)) {
      result.channel_matrix = srs_channel_matrix(sr_cfg.ports.size(),
                                                 static_cast<unsigned>(sr_cfg.resource.nof_antenna_ports));
      return result;
    }
    record_batch();
    cudaStreamSynchronize(stream_);
    finalize_occasion(0, sr_cfg, result);
    return result;
  }

  unsigned estimate_inline_batch(const vram_srs_buffer* const*      vram_bufs,
                                 const srs_estimator_configuration* cfgs,
                                 srs_estimator_result*              results,
                                 unsigned                           nof_occasions) override
  {
    if (!prepare_batch(vram_bufs, cfgs, nof_occasions)) {
      return 0;
    }
    record_batch();
    cudaStreamSynchronize(stream_);
    for (unsigned o = 0; o < batch_n_; ++o) {
      results[o] = srs_estimator_result{};
      finalize_occasion(o, cfgs[o], results[o]);
    }
    return batch_n_;
  }

  bool build_batch_graph(const vram_srs_buffer* const*      vram_bufs,
                         const srs_estimator_configuration* cfgs,
                         unsigned                           nof_occasions) override
  {
    if (graph_exec_) { cudaGraphExecDestroy(graph_exec_); graph_exec_ = nullptr; }
    if (graph_)      { cudaGraphDestroy(graph_);          graph_      = nullptr; }

    if (!prepare_batch(vram_bufs, cfgs, nof_occasions)) {
      return false;
    }
    graph_cfgs_.assign(cfgs, cfgs + batch_n_);

    if (cudaStreamBeginCapture(stream_, cudaStreamCaptureModeThreadLocal) != cudaSuccess) {
      std::fprintf(stderr, "[srs_estimator_inline] stream capture begin failed\n");
      return false;
    }
    record_batch();
    if (cudaStreamEndCapture(stream_, &graph_) != cudaSuccess) {
      std::fprintf(stderr, "[srs_estimator_inline] stream capture end failed\n");
      return false;
    }
    if (cudaGraphInstantiate(&graph_exec_, graph_, 0) != cudaSuccess) {
      std::fprintf(stderr, "[srs_estimator_inline] graph instantiate failed\n");
      cudaGraphDestroy(graph_);
      graph_ = nullptr;
      return false;
    }
    return true;
  }

  unsigned run_batch_graph(srs_estimator_result* results) override
  {
    if (!graph_exec_ || batch_n_ == 0) {
      return 0;
    }
    cudaGraphLaunch(graph_exec_, stream_);
    cudaStreamSynchronize(stream_);
    for (unsigned o = 0; o < batch_n_; ++o) {
      results[o] = srs_estimator_result{};
      finalize_occasion(o, graph_cfgs_[o], results[o]);
    }
    return batch_n_;
  }

  unsigned run_batch_graph_async() override
  {
    if (!graph_exec_ || batch_n_ == 0) {
      return 0;
    }
    cudaGraphLaunch(graph_exec_, stream_);
    return batch_n_;
  }

private:

  bool prepare_batch(const vram_srs_buffer* const*      bufs,
                     const srs_estimator_configuration* cfgs,
                     unsigned                           nof_occasions)
  {
    batch_n_ = std::min(nof_occasions, max_batch_);
    if (batch_n_ == 0) {
      return false;
    }
    const unsigned max_seq = cfg_.max_sequence_length;
    bool           any_valid = false;
    for (unsigned o = 0; o < batch_n_; ++o) {
      valid_[o]            = 0;
      srs_occ_desc& d      = h_desc_[o];
      std::memset(&d, 0, sizeof(d));
      const srs_estimator_configuration& sr_cfg = cfgs[o];

      const unsigned nof_rx      = sr_cfg.ports.size();
      const unsigned nof_tx      = static_cast<unsigned>(sr_cfg.resource.nof_antenna_ports);
      const unsigned nof_symbols = static_cast<unsigned>(sr_cfg.resource.nof_symbols);
      const auto     info0       = get_srs_information(sr_cfg.resource, 0);
      const unsigned seq_len     = info0.sequence_length;
      const bool     interleaved = (nof_tx == 4) && (info0.n_cs >= info0.n_cs_max / 2);
      const unsigned num_groups  = interleaved ? 2 : 1;

      if (nof_rx == 0 || nof_tx == 0 || nof_symbols == 0 || seq_len == 0 || nof_rx > cfg_.max_nof_rx_ports ||
          nof_tx > cfg_.max_nof_antenna_ports || nof_symbols > cfg_.max_nof_symbols ||
          seq_len > cfg_.max_sequence_length) {
        d.nof_rx = 0;
        continue;
      }

      const auto     scs       = to_subcarrier_spacing(sr_cfg.slot.numerology());
      const unsigned comb_size = info0.comb_size;
      const unsigned dft_size  = compute_dft_size(seq_len);
      const double   sampling_rate_Hz =
          static_cast<double>(dft_size) * scs_to_khz(scs) * 1000.0 * comb_size;
      const double max_ta =
          1.0 / static_cast<double>(info0.n_cs_max * scs_to_khz(scs) * 1000 * comb_size);
      const phy_time_unit half_cp = phy_time_unit::from_units_of_kappa(144) / pow2(to_numerology_value(scs) + 1);
      unsigned max_ta_samples = static_cast<unsigned>(std::floor(half_cp.to_seconds() * sampling_rate_Hz));
      if (std::isnormal(max_ta)) {
        max_ta_samples = std::min(max_ta_samples, static_cast<unsigned>(std::floor(max_ta * sampling_rate_Hz)));
      }
      if (max_ta_samples == 0u) {
        max_ta_samples = 1u;
      }

      d.vram_base         = bufs[o]->device_base();
      d.vram_sym_stride   = bufs[o]->get_config().sequence_length;
      d.vram_port_stride  = bufs[o]->get_config().nof_symbols * d.vram_sym_stride;
      d.nof_rx            = nof_rx;
      d.nof_tx            = nof_tx;
      d.nof_symbols       = nof_symbols;
      d.seq_len           = seq_len;
      d.num_groups        = num_groups;
      d.dft_size          = dft_size;
      d.max_ta_samples    = max_ta_samples;
      d.comb_size         = comb_size;
      d.compute_frac      = (dft_size != max_dft_size_) ? 1u : 0u;
      d.inv_sampling_rate = static_cast<float>(1.0 / sampling_rate_Hz);
      for (unsigned r = 0; r < nof_rx; ++r) {
        d.port_map[r] = static_cast<uint8_t>(r);
      }
      for (unsigned t = 0; t < nof_tx; ++t) {
        d.noise_group[t]  = (t == 0) ? 0 : (interleaved && t == 1) ? 1 : -1;
        auto info_tx      = get_srs_information(sr_cfg.resource, t);
        d.ratio_per_tx[t] = static_cast<float>(info_tx.mapping_initial_subcarrier) / static_cast<float>(comb_size);
      }
      d.ratio_per_group[0] = 0.0f;
      d.ratio_per_group[1] = 0.0f;
      for (unsigned t = 0; t < nof_tx; ++t) {
        const int g = d.noise_group[t];
        if (g >= 0 && static_cast<unsigned>(g) < num_groups) {
          d.ratio_per_group[g] = d.ratio_per_tx[t];
        }
      }

      cf_t* h_zc = h_zc_o(o);
      for (unsigned t = 0; t < nof_tx; ++t) {
        auto       info = get_srs_information(sr_cfg.resource, t);
        span<cf_t> dst(h_zc + static_cast<size_t>(t) * max_seq, seq_len);
        lpg_->generate(dst, info.sequence_group, info.sequence_number, info.n_cs, info.n_cs_max);
      }

      valid_[o] = 1;
      if (!any_valid) {
        batch_rx_  = nof_rx;
        batch_dft_ = dft_size;
        any_valid  = true;
      }
    }
    if (!any_valid) {
      return false;
    }
    return ensure_batch_plan(batch_n_, batch_rx_, batch_dft_);
  }

  void record_batch()
  {
    const unsigned n       = batch_n_;
    const unsigned max_rx  = cfg_.max_nof_rx_ports;
    const unsigned max_tx  = cfg_.max_nof_antenna_ports;
    const unsigned max_seq = cfg_.max_sequence_length;

    cudaMemcpyAsync(d_desc_, h_desc_, n * sizeof(srs_occ_desc), cudaMemcpyHostToDevice, stream_);
    cudaMemcpyAsync(d_zc_seq_, h_zc_seq_, n * max_zc_elems() * CF_BYTES, cudaMemcpyHostToDevice, stream_);

    cudaMemsetAsync(d_epre_, 0, n * max_epre_elems() * sizeof(float), stream_);
    cudaMemsetAsync(d_correlation_, 0, static_cast<size_t>(n) * max_dft_size_ * sizeof(float), stream_);

    launch_srs_lse_noise_accum_batch(d_temp_lse_, d_temp_noise_, d_epre_, d_desc_, n, max_rx, max_tx, max_seq, stream_);
    launch_srs_prod_conj_scale_batch(d_temp_lse_, d_zc_seq_, d_desc_, n, max_rx, max_tx, max_seq, stream_);

    for (unsigned tx = 0; tx < max_tx; ++tx) {
      launch_srs_ta_pack_idft_input_batch(d_idft_in_, d_temp_lse_, d_desc_, tx, n, max_rx, max_tx, max_seq,
                                          batch_rx_, batch_dft_, stream_);
      cufftExecC2C(batch_plan_, reinterpret_cast<cufftComplex*>(d_idft_in_),
                   reinterpret_cast<cufftComplex*>(d_idft_out_), CUFFT_INVERSE);
      launch_srs_ta_modsq_accum_batch(d_correlation_, d_idft_out_, d_desc_, n, max_dft_size_, batch_rx_,
                                      batch_dft_, stream_);
    }

    launch_srs_ta_extract_batch(d_correlation_, d_desc_, d_phase_shift_subcarrier_, d_phase_per_tx_,
                                d_phase_per_group_, d_time_alignment_, n, max_tx, max_dft_size_, stream_);
    launch_srs_phase_compensate_batch(d_temp_lse_, d_phase_per_tx_, d_phase_shift_subcarrier_, d_desc_, n,
                                      max_rx, max_tx, max_seq, stream_);
    launch_srs_phase_compensate_noise_batch(d_temp_noise_, d_phase_per_group_, d_phase_shift_subcarrier_,
                                            d_desc_, n, max_rx, max_seq, stream_);
    launch_srs_wideband_coeff_batch(d_channel_coeffs_, d_temp_lse_, d_desc_, n, max_rx, max_tx, max_seq, stream_);
    launch_srs_signal_subtract_and_noise_batch(d_temp_noise_, d_nsq_, d_zc_seq_, d_channel_coeffs_, d_desc_, n,
                                               max_rx, max_tx, max_seq, stream_);

    cudaMemcpyAsync(h_channel_coeffs_, d_channel_coeffs_, n * max_coeff_elems() * CF_BYTES, cudaMemcpyDeviceToHost, stream_);
    cudaMemcpyAsync(h_epre_, d_epre_, n * max_epre_elems() * sizeof(float), cudaMemcpyDeviceToHost, stream_);
    cudaMemcpyAsync(h_nsq_, d_nsq_, n * max_epre_elems() * sizeof(float), cudaMemcpyDeviceToHost, stream_);
    cudaMemcpyAsync(h_time_alignment_, d_time_alignment_, n * sizeof(float), cudaMemcpyDeviceToHost, stream_);
  }

  void finalize_occasion(unsigned o, const srs_estimator_configuration& sr_cfg, srs_estimator_result& result)
  {
    const unsigned nof_rx      = sr_cfg.ports.size();
    const unsigned nof_tx      = static_cast<unsigned>(sr_cfg.resource.nof_antenna_ports);
    const unsigned nof_symbols = static_cast<unsigned>(sr_cfg.resource.nof_symbols);
    const auto     info0       = get_srs_information(sr_cfg.resource, 0);
    const unsigned seq_len     = info0.sequence_length;
    const bool     interleaved = (nof_tx == 4) && (info0.n_cs >= info0.n_cs_max / 2);
    const unsigned num_groups  = interleaved ? 2 : 1;
    const unsigned max_tx      = cfg_.max_nof_antenna_ports;

    result.time_alignment.time_alignment = 0.0;
    result.time_alignment.min            = std::numeric_limits<double>::min();
    result.time_alignment.max            = std::numeric_limits<double>::max();
    result.time_alignment.resolution     = 0.0;

    if (!valid_[o] || nof_rx == 0 || nof_tx == 0) {
      result.channel_matrix = srs_channel_matrix(nof_rx, nof_tx);
      return;
    }

    const auto     scs       = to_subcarrier_spacing(sr_cfg.slot.numerology());
    const unsigned comb_size = info0.comb_size;
    const unsigned dft_size  = compute_dft_size(seq_len);
    const double   sampling_rate_Hz =
        static_cast<double>(dft_size) * scs_to_khz(scs) * 1000.0 * comb_size;
    const double max_ta =
        1.0 / static_cast<double>(info0.n_cs_max * scs_to_khz(scs) * 1000 * comb_size);
    const phy_time_unit half_cp = phy_time_unit::from_units_of_kappa(144) / pow2(to_numerology_value(scs) + 1);
    unsigned max_ta_samples = static_cast<unsigned>(std::floor(half_cp.to_seconds() * sampling_rate_Hz));
    if (std::isnormal(max_ta)) {
      max_ta_samples = std::min(max_ta_samples, static_cast<unsigned>(std::floor(max_ta * sampling_rate_Hz)));
    }
    if (max_ta_samples == 0u) {
      max_ta_samples = 1u;
    }
    result.time_alignment.resolution     = 1.0 / sampling_rate_Hz;
    result.time_alignment.min            = -static_cast<double>(max_ta_samples) / sampling_rate_Hz;
    result.time_alignment.max            = static_cast<double>(max_ta_samples) / sampling_rate_Hz;
    result.time_alignment.time_alignment = static_cast<double>(*h_ta_o(o));

    srs_channel_matrix channel_matrix(nof_rx, nof_tx);
    float              rsrp   = 0.0f;
    const cf_t*        coeffs = h_coeffs_o(o);
    for (unsigned r = 0; r < nof_rx; ++r) {
      for (unsigned t = 0; t < nof_tx; ++t) {
        const cf_t c = coeffs[r * max_tx + t];
        channel_matrix.set_coefficient(c, r, t);
        rsrp += std::norm(c);
      }
    }

    float        noise_var = 0.0f;
    const float* nsq       = h_nsq_o(o);
    for (unsigned r = 0; r < nof_rx; ++r) {
      for (unsigned g = 0; g < num_groups; ++g) {
        noise_var += nsq[r * 2 + g];
      }
    }
    const unsigned nof_estimates     = interleaved ? 2 : nof_tx;
    const unsigned correction_factor = interleaved ? 2 : 1;
    noise_var /= static_cast<float>((nof_symbols * seq_len - nof_estimates) * correction_factor * nof_rx);

    const float noise_std = std::max(std::sqrt(noise_var), std::sqrt(rsrp) * 0.01f);
    channel_matrix *= 1.0f / noise_std;

    float        epre    = 0.0f;
    const float* epre_in = h_epre_o(o);
    for (unsigned r = 0; r < nof_rx; ++r) {
      for (unsigned g = 0; g < num_groups; ++g) {
        epre += epre_in[r * 2 + g];
      }
    }
    epre /= static_cast<float>(nof_symbols * correction_factor * nof_rx);
    rsrp /= static_cast<float>(nof_tx * nof_rx);

    result.channel_matrix = std::move(channel_matrix);
    result.noise_variance = noise_var;
    result.epre_dB        = convert_power_to_dB(epre);
    result.rsrp_dB        = convert_power_to_dB(rsrp);
  }

  bool ensure_batch_plan(unsigned n, unsigned rx, unsigned dft)
  {
    if (batch_plan_ && plan_n_ == n && plan_rx_ == rx && plan_dft_ == dft) {
      return true;
    }
    if (batch_plan_) {
      cufftDestroy(batch_plan_);
      batch_plan_ = 0;
    }
    int         nn[1]      = {static_cast<int>(dft)};
    int         inembed[]  = {static_cast<int>(dft)};
    int         onembed[]  = {static_cast<int>(dft)};
    cufftResult fr = cufftPlanMany(&batch_plan_, 1, nn, inembed, 1, dft, onembed, 1, dft, CUFFT_C2C,
                                   static_cast<int>(n * rx));
    if (fr != CUFFT_SUCCESS) {
      std::fprintf(stderr, "[srs_estimator_inline] cufftPlanMany(batch=%u, dft=%u) failed: %d\n", n * rx, dft,
                   static_cast<int>(fr));
      batch_plan_ = 0;
      return false;
    }
    cufftSetStream(batch_plan_, stream_);
    plan_n_   = n;
    plan_rx_  = rx;
    plan_dft_ = dft;
    return true;
  }

  bool alloc_fail(const char* name)
  {
    std::fprintf(stderr, "[srs_estimator_inline] cudaMalloc(%s) failed\n", name);
    return false;
  }

  void free_scratch()
  {
    auto fdev = [](void*& p) { if (p) { cudaFree(p); p = nullptr; } };
    auto fhst = [](void*& p) { if (p) { cudaFreeHost(p); p = nullptr; } };
    fdev(d_temp_lse_); fdev(d_temp_noise_); fdev(d_zc_seq_); fdev(d_channel_coeffs_);
    fdev(reinterpret_cast<void*&>(d_epre_));
    fdev(reinterpret_cast<void*&>(d_nsq_));
    fdev(d_idft_in_); fdev(d_idft_out_);
    fdev(reinterpret_cast<void*&>(d_correlation_));
    fdev(reinterpret_cast<void*&>(d_phase_per_tx_));
    fdev(reinterpret_cast<void*&>(d_phase_per_group_));
    fdev(reinterpret_cast<void*&>(d_phase_shift_subcarrier_));
    fdev(reinterpret_cast<void*&>(d_time_alignment_));
    fdev(reinterpret_cast<void*&>(d_desc_));
    fhst(h_zc_seq_); fhst(h_channel_coeffs_);
    fhst(reinterpret_cast<void*&>(h_epre_));
    fhst(reinterpret_cast<void*&>(h_nsq_));
    fhst(reinterpret_cast<void*&>(h_time_alignment_));
    fhst(reinterpret_cast<void*&>(h_desc_));
  }

  unsigned compute_dft_size(unsigned seq_len) const
  {
    unsigned nof_required_re = (seq_len * max_dft_size_) / static_cast<unsigned>(MAX_NOF_SUBCARRIERS);
    unsigned dft             = pow2(log2_ceil(nof_required_re));
    return std::max(min_dft_size_, dft);
  }

  size_t max_lse_elems() const   { return static_cast<size_t>(cfg_.max_nof_rx_ports) * cfg_.max_nof_antenna_ports * cfg_.max_sequence_length; }
  size_t max_noise_elems() const { return static_cast<size_t>(2) * cfg_.max_nof_rx_ports * cfg_.max_sequence_length; }
  size_t max_zc_elems() const    { return static_cast<size_t>(cfg_.max_nof_antenna_ports) * cfg_.max_sequence_length; }
  size_t max_coeff_elems() const { return static_cast<size_t>(cfg_.max_nof_rx_ports) * cfg_.max_nof_antenna_ports; }
  size_t max_epre_elems() const  { return static_cast<size_t>(cfg_.max_nof_rx_ports) * 2; }

  cf_t*  h_zc_o(unsigned o)     const { return reinterpret_cast<cf_t*>(h_zc_seq_) + static_cast<size_t>(o) * max_zc_elems(); }
  cf_t*  h_coeffs_o(unsigned o) const { return reinterpret_cast<cf_t*>(h_channel_coeffs_) + static_cast<size_t>(o) * max_coeff_elems(); }
  float* h_epre_o(unsigned o)   const { return h_epre_ + static_cast<size_t>(o) * max_epre_elems(); }
  float* h_nsq_o(unsigned o)    const { return h_nsq_ + static_cast<size_t>(o) * max_epre_elems(); }
  float* h_ta_o(unsigned o)     const { return h_time_alignment_ + o; }

  config       cfg_{};
  cudaStream_t stream_     = nullptr;
  unsigned     max_batch_  = 1;

  void*  d_temp_lse_       = nullptr;
  void*  d_temp_noise_     = nullptr;
  void*  d_zc_seq_         = nullptr;
  void*  d_channel_coeffs_ = nullptr;
  float* d_epre_           = nullptr;
  float* d_nsq_            = nullptr;
  void*  d_idft_in_        = nullptr;
  void*  d_idft_out_       = nullptr;
  float* d_correlation_    = nullptr;
  float* d_phase_per_tx_   = nullptr;
  float* d_phase_per_group_= nullptr;
  float* d_phase_shift_subcarrier_ = nullptr;
  float* d_time_alignment_ = nullptr;
  srs_occ_desc* d_desc_    = nullptr;

  void*  h_zc_seq_         = nullptr;
  void*  h_channel_coeffs_ = nullptr;
  float* h_epre_           = nullptr;
  float* h_nsq_            = nullptr;
  float* h_time_alignment_ = nullptr;
  srs_occ_desc* h_desc_    = nullptr;

  unsigned min_dft_size_ = 0;
  unsigned max_dft_size_ = 0;

  cufftHandle batch_plan_ = 0;
  unsigned    plan_n_     = 0;
  unsigned    plan_rx_    = 0;
  unsigned    plan_dft_   = 0;

  unsigned          batch_n_   = 0;
  unsigned          batch_rx_  = 0;
  unsigned          batch_dft_ = 0;
  std::vector<char> valid_;

  cudaGraph_t                              graph_      = nullptr;
  cudaGraphExec_t                          graph_exec_ = nullptr;
  std::vector<srs_estimator_configuration> graph_cfgs_;

  std::unique_ptr<low_papr_sequence_generator> lpg_;
};

}

std::unique_ptr<srs_estimator_inline> srs_estimator_inline::create(const config& cfg)
{
  if (cfg.max_nof_rx_ports == 0 || cfg.max_nof_antenna_ports == 0 || cfg.max_nof_symbols == 0 ||
      cfg.max_sequence_length == 0) {
    std::fprintf(stderr, "[srs_estimator_inline] invalid create config\n");
    return nullptr;
  }
  if (cfg.max_nof_antenna_ports > 4) {
    std::fprintf(stderr, "[srs_estimator_inline] max_nof_antenna_ports>4 not supported\n");
    return nullptr;
  }
  std::unique_ptr<srs_estimator_inline_impl> obj{new (std::nothrow) srs_estimator_inline_impl(cfg)};
  if (!obj || !obj->init()) {
    return nullptr;
  }
  return obj;
}

}
}
}
