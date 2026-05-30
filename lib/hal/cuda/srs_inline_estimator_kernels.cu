#include "srs_inline_estimator_kernels.h"

#include <cstdio>
#include <cuda_runtime.h>

namespace ocudu {
namespace hal {
namespace cuda {

namespace {

constexpr unsigned BLOCK_SIZE = 128;

constexpr float    K_TWOPI          = 6.283185307179586f;
constexpr unsigned K_CEXP_TABLE_LEN = 1024;

__device__ __forceinline__ float2 cexp_quantized(float theta)
{
  int idx = static_cast<int>(roundf(theta * (static_cast<float>(K_CEXP_TABLE_LEN) / K_TWOPI)));
  idx     = ((idx % static_cast<int>(K_CEXP_TABLE_LEN)) + static_cast<int>(K_CEXP_TABLE_LEN)) %
        static_cast<int>(K_CEXP_TABLE_LEN);
  float tq = K_TWOPI * static_cast<float>(idx) / static_cast<float>(K_CEXP_TABLE_LEN);
  float s, c;
  sincosf(tq, &s, &c);
  return {c, s};
}

__device__ __forceinline__ float2 cf_add(float2 a, float2 b) { return {a.x + b.x, a.y + b.y}; }
__device__ __forceinline__ float2 cf_sub(float2 a, float2 b) { return {a.x - b.x, a.y - b.y}; }
__device__ __forceinline__ float2 cf_mul(float2 a, float2 b)
{
  return {a.x * b.x - a.y * b.y, a.x * b.y + a.y * b.x};
}
__device__ __forceinline__ float2 cf_mul_conj(float2 a, float2 b)
{
  return {a.x * b.x + a.y * b.y, a.y * b.x - a.x * b.y};
}
__device__ __forceinline__ float2 cf_scale(float2 a, float s) { return {a.x * s, a.y * s}; }
__device__ __forceinline__ float  cf_norm2(float2 a) { return a.x * a.x + a.y * a.y; }

__device__ __forceinline__ float2 warp_reduce_sum_cf(float2 v)
{
  for (int off = 16; off > 0; off >>= 1) {
    v.x += __shfl_down_sync(0xffffffff, v.x, off);
    v.y += __shfl_down_sync(0xffffffff, v.y, off);
  }
  return v;
}
__device__ __forceinline__ float warp_reduce_sum_f(float v)
{
  for (int off = 16; off > 0; off >>= 1) {
    v += __shfl_down_sync(0xffffffff, v, off);
  }
  return v;
}

__device__ __forceinline__ size_t lse_idx(unsigned o, unsigned rx, unsigned tx, unsigned max_rx, unsigned max_tx, unsigned max_seq)
{
  return ((static_cast<size_t>(o) * max_rx + rx) * max_tx + tx) * max_seq;
}
__device__ __forceinline__ size_t noise_idx(unsigned o, unsigned grp, unsigned rx, unsigned max_rx, unsigned max_seq)
{
  return ((static_cast<size_t>(o) * 2 + grp) * max_rx + rx) * max_seq;
}
__device__ __forceinline__ size_t zc_idx(unsigned o, unsigned tx, unsigned max_tx, unsigned max_seq)
{
  return (static_cast<size_t>(o) * max_tx + tx) * max_seq;
}

__global__ void k_lse_noise_accum(float2*       __restrict__ d_lse,
                                  float2*       __restrict__ d_noise,
                                  float*        __restrict__ d_epre,
                                  const srs_occ_desc* __restrict__ descs,
                                  unsigned max_rx,
                                  unsigned max_tx,
                                  unsigned max_seq)
{
  const unsigned       o  = blockIdx.z;
  const srs_occ_desc&  d  = descs[o];
  const unsigned       rx = blockIdx.x;
  const unsigned       tx = blockIdx.y;
  if (rx >= d.nof_rx || tx >= d.nof_tx) {
    return;
  }
  const unsigned nof_symbols = d.nof_symbols;
  const unsigned seq_len      = d.seq_len;
  const float2*  vram         = reinterpret_cast<const float2*>(d.vram_base);
  const unsigned vram_port    = d.port_map[rx];
  const int      ng           = d.noise_group[tx];
  const bool     accum_noise  = (ng >= 0);

  float2* lse_base   = d_lse + lse_idx(o, rx, tx, max_rx, max_tx, max_seq);
  float2* noise_base = accum_noise ? d_noise + noise_idx(o, static_cast<unsigned>(ng), rx, max_rx, max_seq) : nullptr;

  float thread_epre = 0.0f;
  for (unsigned i = threadIdx.x; i < seq_len; i += BLOCK_SIZE) {
    float2 lse_acc{0.0f, 0.0f};
    float2 noise_acc{0.0f, 0.0f};
    float  epre_acc = 0.0f;
    for (unsigned s = 0; s < nof_symbols; ++s) {
      const float2 sample =
          vram[static_cast<size_t>(vram_port) * d.vram_port_stride + s * d.vram_sym_stride + i];
      lse_acc = cf_add(lse_acc, sample);
      if (accum_noise) {
        noise_acc = cf_add(noise_acc, sample);
        epre_acc += cf_norm2(sample);
      }
    }
    lse_base[i] = lse_acc;
    if (accum_noise) {
      noise_base[i] = noise_acc;
    }
    thread_epre += epre_acc;
  }
  if (accum_noise) {
    thread_epre /= static_cast<float>(seq_len);
    float reduced = warp_reduce_sum_f(thread_epre);
    if ((threadIdx.x & 31u) == 0u) {
      atomicAdd(&d_epre[static_cast<size_t>(o) * max_rx * 2 + rx * 2 + ng], reduced);
    }
  }
}

__global__ void k_prod_conj_scale(float2*       __restrict__ d_lse,
                                  const float2* __restrict__ d_zc,
                                  const srs_occ_desc* __restrict__ descs,
                                  unsigned max_rx,
                                  unsigned max_tx,
                                  unsigned max_seq)
{
  const unsigned      o  = blockIdx.z;
  const srs_occ_desc& d  = descs[o];
  const unsigned      rx = blockIdx.x;
  const unsigned      tx = blockIdx.y;
  if (rx >= d.nof_rx || tx >= d.nof_tx) {
    return;
  }
  const unsigned seq_len = d.seq_len;
  const float    inv     = 1.0f / static_cast<float>(d.nof_symbols);
  float2*        lse_base = d_lse + lse_idx(o, rx, tx, max_rx, max_tx, max_seq);
  const float2*  zc_base  = d_zc + zc_idx(o, tx, max_tx, max_seq);
  for (unsigned i = threadIdx.x; i < seq_len; i += BLOCK_SIZE) {
    lse_base[i] = cf_scale(cf_mul_conj(lse_base[i], zc_base[i]), inv);
  }
}

__global__ void k_pack_idft(float2*       __restrict__ d_idft_dense,
                            const float2* __restrict__ d_lse,
                            const srs_occ_desc* __restrict__ descs,
                            unsigned tx_port,
                            unsigned max_rx,
                            unsigned max_tx,
                            unsigned max_seq,
                            unsigned batch_rx,
                            unsigned batch_dft)
{
  const unsigned      o  = blockIdx.y;
  const unsigned      rx = blockIdx.x;
  const srs_occ_desc& d  = descs[o];
  if (rx >= d.nof_rx) {
    return;
  }
  const unsigned seq_len  = d.seq_len;
  float2*        out_base = d_idft_dense + (static_cast<size_t>(o) * batch_rx + rx) * batch_dft;
  const float2*  in_base  = d_lse + lse_idx(o, rx, tx_port, max_rx, max_tx, max_seq);
  for (unsigned i = threadIdx.x; i < batch_dft; i += BLOCK_SIZE) {
    out_base[i] = (i < seq_len) ? in_base[i] : float2{0.0f, 0.0f};
  }
}

__global__ void k_ta_modsq_accum(float*        __restrict__ d_corr,
                                 const float2* __restrict__ d_idft_dense,
                                 const srs_occ_desc* __restrict__ descs,
                                 unsigned max_dft,
                                 unsigned batch_rx,
                                 unsigned batch_dft)
{
  const unsigned      o   = blockIdx.y;
  const unsigned      bin = blockIdx.x * blockDim.x + threadIdx.x;
  const srs_occ_desc& d   = descs[o];
  if (bin >= d.dft_size) {
    return;
  }
  float acc = 0.0f;
  for (unsigned rx = 0; rx < d.nof_rx; ++rx) {
    float2 z = d_idft_dense[(static_cast<size_t>(o) * batch_rx + rx) * batch_dft + bin];
    acc += z.x * z.x + z.y * z.y;
  }
  d_corr[static_cast<size_t>(o) * max_dft + bin] += acc;
}

__global__ void k_ta_extract(const float* __restrict__ d_corr,
                             const srs_occ_desc* __restrict__ descs,
                             float* __restrict__ d_psc,
                             float* __restrict__ d_phtx,
                             float* __restrict__ d_phgrp,
                             float* __restrict__ d_ta,
                             unsigned max_tx,
                             unsigned max_dft)
{
  const unsigned o = blockIdx.x;
  if (threadIdx.x != 0) {
    return;
  }
  const srs_occ_desc& d              = descs[o];
  const unsigned      dft_size       = d.dft_size;
  const unsigned      max_ta_samples = d.max_ta_samples;
  const float*        corr           = d_corr + static_cast<size_t>(o) * max_dft;

  unsigned dpos = 0;
  float    dval = 0.0f;
  for (unsigned i = 0; i < max_ta_samples; ++i) {
    float v = corr[i];
    if (v > dval) { dval = v; dpos = i; }
  }
  unsigned apos = 0;
  float    aval = 0.0f;
  for (unsigned i = 0; i < max_ta_samples; ++i) {
    float v = corr[dft_size - max_ta_samples + i];
    if (v > aval) { aval = v; apos = i; }
  }
  int idx = -(static_cast<int>(max_ta_samples) - static_cast<int>(apos));
  if (dval >= aval) {
    idx = static_cast<int>(dpos);
  }

  float frac = 0.0f;
  if (d.compute_frac) {
    const unsigned nof_taps = (max_ta_samples > 2) ? 5u : 3u;
    float          taps[5];
    const int      dd = static_cast<int>(dft_size);
    for (unsigned t = 0; t < nof_taps; ++t) {
      int ii  = (idx + static_cast<int>(t) + dd - static_cast<int>(nof_taps / 2)) % dd;
      ii      = (ii + dd) % dd;
      taps[t] = corr[ii];
    }
    float num = 0.0f, den = 0.0f, correction;
    if (nof_taps == 5) {
      const float nw[5] = {-0.400000f, -0.200000f, 0.000000f, 0.200000f, 0.400000f};
      const float dw[5] = {0.571429f, -0.285714f, -0.571429f, -0.285714f, 0.571429f};
      correction        = 1.0f;
      for (int k = 0; k < 5; ++k) { num += nw[k] * taps[k]; den += dw[k] * taps[k]; }
    } else {
      const float nw[3] = {-0.5f, 0.0f, 0.5f};
      const float dw[3] = {0.5f, -1.0f, 0.5f};
      correction        = 0.5f;
      for (int k = 0; k < 3; ++k) { num += nw[k] * taps[k]; den += dw[k] * taps[k]; }
    }
    float r = -correction * num / den;
    if (!isnan(r) && !isinf(r) && fabsf(r) <= 1.0f) {
      frac = r;
    }
  }

  const float idx_frac = static_cast<float>(idx) + frac;
  const float psc      = K_TWOPI * idx_frac / static_cast<float>(dft_size);
  d_psc[o] = psc;
  d_ta[o]  = idx_frac * d.inv_sampling_rate;
  for (unsigned tx = 0; tx < d.nof_tx; ++tx) {
    d_phtx[static_cast<size_t>(o) * max_tx + tx] = psc * d.ratio_per_tx[tx];
  }
  for (unsigned g = 0; g < d.num_groups; ++g) {
    d_phgrp[static_cast<size_t>(o) * 2 + g] = psc * d.ratio_per_group[g];
  }
}

__global__ void k_phase_compensate(float2*      __restrict__ d_lse,
                                   const float* __restrict__ d_phtx,
                                   const float* __restrict__ d_psc,
                                   const srs_occ_desc* __restrict__ descs,
                                   unsigned max_rx,
                                   unsigned max_tx,
                                   unsigned max_seq)
{
  const unsigned      o  = blockIdx.z;
  const srs_occ_desc& d  = descs[o];
  const unsigned      rx = blockIdx.x;
  const unsigned      tx = blockIdx.y;
  if (rx >= d.nof_rx || tx >= d.nof_tx) {
    return;
  }
  const unsigned seq_len = d.seq_len;
  float2*        base    = d_lse + lse_idx(o, rx, tx, max_rx, max_tx, max_seq);
  const float    psc     = d_psc[o];
  const float    off     = d_phtx[static_cast<size_t>(o) * max_tx + tx];
  for (unsigned i = threadIdx.x; i < seq_len; i += BLOCK_SIZE) {
    const float  theta = psc * static_cast<float>(i) + off;
    const float2 e     = cexp_quantized(theta);
    float2       v     = base[i];
    base[i].x          = v.x * e.x - v.y * e.y;
    base[i].y          = v.x * e.y + v.y * e.x;
  }
}

__global__ void k_phase_compensate_noise(float2*      __restrict__ d_noise,
                                         const float* __restrict__ d_phgrp,
                                         const float* __restrict__ d_psc,
                                         const srs_occ_desc* __restrict__ descs,
                                         unsigned max_rx,
                                         unsigned max_seq)
{
  const unsigned      o  = blockIdx.y;
  const srs_occ_desc& d  = descs[o];
  const unsigned      rx = blockIdx.x;
  if (rx >= d.nof_rx) {
    return;
  }
  const unsigned seq_len    = d.seq_len;
  const unsigned num_groups = d.num_groups;
  const float    psc        = d_psc[o];
  for (unsigned g = 0; g < num_groups; ++g) {
    float2*     base = d_noise + noise_idx(o, g, rx, max_rx, max_seq);
    const float off  = d_phgrp[static_cast<size_t>(o) * 2 + g];
    for (unsigned i = threadIdx.x; i < seq_len; i += BLOCK_SIZE) {
      const float  theta = psc * static_cast<float>(i) + off;
      const float2 e     = cexp_quantized(theta);
      float2       v     = base[i];
      base[i].x          = v.x * e.x - v.y * e.y;
      base[i].y          = v.x * e.y + v.y * e.x;
    }
  }
}

__global__ void k_wideband_coeff(float2*       __restrict__ d_coeffs,
                                 const float2* __restrict__ d_lse,
                                 const srs_occ_desc* __restrict__ descs,
                                 unsigned max_rx,
                                 unsigned max_tx,
                                 unsigned max_seq)
{
  const unsigned      o  = blockIdx.z;
  const srs_occ_desc& d  = descs[o];
  const unsigned      rx = blockIdx.x;
  const unsigned      tx = blockIdx.y;
  if (rx >= d.nof_rx || tx >= d.nof_tx) {
    return;
  }
  const unsigned seq_len  = d.seq_len;
  const float2*  lse_base = d_lse + lse_idx(o, rx, tx, max_rx, max_tx, max_seq);

  constexpr unsigned NWARPS = BLOCK_SIZE / 32;
  __shared__ float2  warp_sums[NWARPS];

  float2 thread_sum{0.0f, 0.0f};
  for (unsigned i = threadIdx.x; i < seq_len; i += BLOCK_SIZE) {
    thread_sum = cf_add(thread_sum, lse_base[i]);
  }
  float2 wsum = warp_reduce_sum_cf(thread_sum);
  if ((threadIdx.x & 31u) == 0u) {
    warp_sums[threadIdx.x >> 5] = wsum;
  }
  __syncthreads();
  if (threadIdx.x == 0) {
    float2 final_sum = warp_sums[0];
    for (unsigned w = 1; w < NWARPS; ++w) {
      final_sum = cf_add(final_sum, warp_sums[w]);
    }
    d_coeffs[static_cast<size_t>(o) * max_rx * max_tx + rx * max_tx + tx] =
        cf_scale(final_sum, 1.0f / static_cast<float>(seq_len));
  }
}

__global__ void k_signal_subtract_and_noise(float2*       __restrict__ d_noise,
                                            float*        __restrict__ d_nsq,
                                            const float2* __restrict__ d_zc,
                                            const float2* __restrict__ d_coeffs,
                                            const srs_occ_desc* __restrict__ descs,
                                            unsigned max_rx,
                                            unsigned max_tx,
                                            unsigned max_seq)
{
  const unsigned      o  = blockIdx.y;
  const srs_occ_desc& d  = descs[o];
  const unsigned      rx = blockIdx.x;
  if (rx >= d.nof_rx) {
    return;
  }
  const unsigned seq_len     = d.seq_len;
  const unsigned nof_tx      = d.nof_tx;
  const unsigned nof_symbols = d.nof_symbols;
  const unsigned num_groups  = d.num_groups;

  constexpr unsigned NWARPS = BLOCK_SIZE / 32;
  __shared__ float2  coeff_for_group[4];
  __shared__ uint8_t tx_for_group[4];
  __shared__ unsigned nof_active;
  __shared__ float   warp_sumsq[NWARPS];

  for (unsigned g = 0; g < num_groups; ++g) {
    if (threadIdx.x == 0) {
      nof_active = 0;
      for (unsigned tx = 0; tx < nof_tx && nof_active < 4; ++tx) {
        if (d.noise_group[tx] == static_cast<int8_t>(g)) {
          tx_for_group[nof_active]    = static_cast<uint8_t>(tx);
          coeff_for_group[nof_active] = d_coeffs[static_cast<size_t>(o) * max_rx * max_tx + rx * max_tx + tx];
          ++nof_active;
        }
      }
    }
    __syncthreads();

    float2* noise_base = d_noise + noise_idx(o, g, rx, max_rx, max_seq);
    float   thread_sumsq = 0.0f;
    for (unsigned i = threadIdx.x; i < seq_len; i += BLOCK_SIZE) {
      float2 noise_val = noise_base[i];
      for (unsigned k = 0; k < nof_active; ++k) {
        const float2 zc        = d_zc[zc_idx(o, tx_for_group[k], max_tx, max_seq) + i];
        float2       recovered = cf_mul(zc, coeff_for_group[k]);
        recovered              = cf_scale(recovered, static_cast<float>(nof_symbols));
        noise_val              = cf_sub(noise_val, recovered);
      }
      noise_base[i]  = noise_val;
      thread_sumsq  += cf_norm2(noise_val);
    }
    float wsum = warp_reduce_sum_f(thread_sumsq);
    if ((threadIdx.x & 31u) == 0u) {
      warp_sumsq[threadIdx.x >> 5] = wsum;
    }
    __syncthreads();
    if (threadIdx.x == 0) {
      float final_sum = warp_sumsq[0];
      for (unsigned w = 1; w < NWARPS; ++w) {
        final_sum += warp_sumsq[w];
      }
      d_nsq[static_cast<size_t>(o) * max_rx * 2 + rx * 2 + g] = final_sum;
    }
    __syncthreads();
  }
}

}

static const srs_occ_desc* as_descs(const void* p) { return reinterpret_cast<const srs_occ_desc*>(p); }

static int check_launch(const char* name)
{
  if (cudaError_t e = cudaGetLastError(); e != cudaSuccess) {
    std::fprintf(stderr, "[srs_inline_kernels] %s launch failed: %s\n", name, cudaGetErrorString(e));
    return -2;
  }
  return 0;
}

int launch_srs_lse_noise_accum_batch(void* d_lse, void* d_noise, float* d_epre, const void* d_descs,
                                     unsigned nof_occasions, unsigned max_rx, unsigned max_tx,
                                     unsigned max_seq, void* stream)
{
  if (nof_occasions == 0) return 0;
  dim3 grid(max_rx, max_tx, nof_occasions);
  k_lse_noise_accum<<<grid, BLOCK_SIZE, 0, static_cast<cudaStream_t>(stream)>>>(
      reinterpret_cast<float2*>(d_lse), reinterpret_cast<float2*>(d_noise), d_epre, as_descs(d_descs),
      max_rx, max_tx, max_seq);
  return check_launch("lse_noise_accum");
}

int launch_srs_prod_conj_scale_batch(void* d_lse, const void* d_zc, const void* d_descs,
                                     unsigned nof_occasions, unsigned max_rx, unsigned max_tx,
                                     unsigned max_seq, void* stream)
{
  if (nof_occasions == 0) return 0;
  dim3 grid(max_rx, max_tx, nof_occasions);
  k_prod_conj_scale<<<grid, BLOCK_SIZE, 0, static_cast<cudaStream_t>(stream)>>>(
      reinterpret_cast<float2*>(d_lse), reinterpret_cast<const float2*>(d_zc), as_descs(d_descs),
      max_rx, max_tx, max_seq);
  return check_launch("prod_conj_scale");
}

int launch_srs_ta_pack_idft_input_batch(void* d_idft_dense, const void* d_lse, const void* d_descs,
                                        unsigned tx_port, unsigned nof_occasions, unsigned max_rx,
                                        unsigned max_tx, unsigned max_seq, unsigned batch_rx,
                                        unsigned batch_dft, void* stream)
{
  if (nof_occasions == 0) return 0;
  dim3 grid(batch_rx, nof_occasions, 1);
  k_pack_idft<<<grid, BLOCK_SIZE, 0, static_cast<cudaStream_t>(stream)>>>(
      reinterpret_cast<float2*>(d_idft_dense), reinterpret_cast<const float2*>(d_lse), as_descs(d_descs),
      tx_port, max_rx, max_tx, max_seq, batch_rx, batch_dft);
  return check_launch("pack_idft");
}

int launch_srs_ta_modsq_accum_batch(float* d_corr, const void* d_idft_dense, const void* d_descs,
                                    unsigned nof_occasions, unsigned max_dft, unsigned batch_rx,
                                    unsigned batch_dft, void* stream)
{
  if (nof_occasions == 0) return 0;
  const unsigned blocks = (batch_dft + BLOCK_SIZE - 1) / BLOCK_SIZE;
  dim3 grid(blocks, nof_occasions, 1);
  k_ta_modsq_accum<<<grid, BLOCK_SIZE, 0, static_cast<cudaStream_t>(stream)>>>(
      d_corr, reinterpret_cast<const float2*>(d_idft_dense), as_descs(d_descs), max_dft, batch_rx, batch_dft);
  return check_launch("ta_modsq_accum");
}

int launch_srs_ta_extract_batch(const float* d_corr, const void* d_descs, float* d_psc, float* d_phtx,
                                float* d_phgrp, float* d_ta, unsigned nof_occasions, unsigned max_tx,
                                unsigned max_dft, void* stream)
{
  if (nof_occasions == 0) return 0;
  k_ta_extract<<<nof_occasions, 32, 0, static_cast<cudaStream_t>(stream)>>>(
      d_corr, as_descs(d_descs), d_psc, d_phtx, d_phgrp, d_ta, max_tx, max_dft);
  return check_launch("ta_extract");
}

int launch_srs_phase_compensate_batch(void* d_lse, const float* d_phtx, const float* d_psc,
                                      const void* d_descs, unsigned nof_occasions, unsigned max_rx,
                                      unsigned max_tx, unsigned max_seq, void* stream)
{
  if (nof_occasions == 0) return 0;
  dim3 grid(max_rx, max_tx, nof_occasions);
  k_phase_compensate<<<grid, BLOCK_SIZE, 0, static_cast<cudaStream_t>(stream)>>>(
      reinterpret_cast<float2*>(d_lse), d_phtx, d_psc, as_descs(d_descs), max_rx, max_tx, max_seq);
  return check_launch("phase_compensate");
}

int launch_srs_phase_compensate_noise_batch(void* d_noise, const float* d_phgrp, const float* d_psc,
                                            const void* d_descs, unsigned nof_occasions, unsigned max_rx,
                                            unsigned max_seq, void* stream)
{
  if (nof_occasions == 0) return 0;
  dim3 grid(max_rx, nof_occasions, 1);
  k_phase_compensate_noise<<<grid, BLOCK_SIZE, 0, static_cast<cudaStream_t>(stream)>>>(
      reinterpret_cast<float2*>(d_noise), d_phgrp, d_psc, as_descs(d_descs), max_rx, max_seq);
  return check_launch("phase_compensate_noise");
}

int launch_srs_wideband_coeff_batch(void* d_coeffs, const void* d_lse, const void* d_descs,
                                    unsigned nof_occasions, unsigned max_rx, unsigned max_tx,
                                    unsigned max_seq, void* stream)
{
  if (nof_occasions == 0) return 0;
  dim3 grid(max_rx, max_tx, nof_occasions);
  k_wideband_coeff<<<grid, BLOCK_SIZE, 0, static_cast<cudaStream_t>(stream)>>>(
      reinterpret_cast<float2*>(d_coeffs), reinterpret_cast<const float2*>(d_lse), as_descs(d_descs),
      max_rx, max_tx, max_seq);
  return check_launch("wideband_coeff");
}

int launch_srs_signal_subtract_and_noise_batch(void* d_noise, float* d_nsq, const void* d_zc,
                                               const void* d_coeffs, const void* d_descs,
                                               unsigned nof_occasions, unsigned max_rx, unsigned max_tx,
                                               unsigned max_seq, void* stream)
{
  if (nof_occasions == 0) return 0;
  dim3 grid(max_rx, nof_occasions, 1);
  k_signal_subtract_and_noise<<<grid, BLOCK_SIZE, 0, static_cast<cudaStream_t>(stream)>>>(
      reinterpret_cast<float2*>(d_noise), d_nsq, reinterpret_cast<const float2*>(d_zc),
      reinterpret_cast<const float2*>(d_coeffs), as_descs(d_descs), max_rx, max_tx, max_seq);
  return check_launch("signal_subtract_and_noise");
}

}
}
}
