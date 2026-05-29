#include "prach_detector_inline_kernels.h"

#include <cstdio>
#include <cuda_runtime.h>

namespace ocudu {
namespace hal {
namespace cuda {

namespace {

__device__ __forceinline__ float bf16_to_float(unsigned short b)
{
  unsigned int x = static_cast<unsigned int>(b) << 16;
  return __uint_as_float(x);
}

__device__ __forceinline__ float warp_reduce_sum(float v)
{
  for (int off = 16; off > 0; off >>= 1) {
    v += __shfl_down_sync(0xFFFFFFFF, v, off);
  }
  return v;
}
__device__ __forceinline__ float block_reduce_sum(float v, float* shared)
{
  v = warp_reduce_sum(v);
  int lane = threadIdx.x & 31;
  int wid  = threadIdx.x >> 5;
  if (lane == 0) {
    shared[wid] = v;
  }
  __syncthreads();
  int nwarps = (blockDim.x + 31) >> 5;
  v          = (threadIdx.x < nwarps) ? shared[threadIdx.x] : 0.0F;
  if (wid == 0) {
    v = warp_reduce_sum(v);
  }
  return v;
}

__global__ void k_rssi_reduce(float*               __restrict__ d_rssi,
                              const unsigned short* __restrict__ d_samples,
                              unsigned             nof_rx_ports,
                              unsigned             nof_symbols,
                              unsigned             L_ra,
                              unsigned             sym_stride_elems,
                              unsigned             ps_stride_elems)
{
  const unsigned tuple_idx = blockIdx.x;
  const unsigned i_port    = tuple_idx / nof_symbols;
  const unsigned i_symbol  = tuple_idx % nof_symbols;
  if (i_port >= nof_rx_ports) {
    return;
  }
  const unsigned        base_elem = i_port * ps_stride_elems + i_symbol * sym_stride_elems;
  const unsigned short* p         = d_samples + base_elem * 2u;

  float                sum = 0.0F;
  for (unsigned i = threadIdx.x; i < L_ra; i += blockDim.x) {
    float re = bf16_to_float(p[2 * i]);
    float im = bf16_to_float(p[2 * i + 1]);
    sum += re * re + im * im;
  }
  __shared__ float shared[32];
  sum = block_reduce_sum(sum, shared);
  if (threadIdx.x == 0) {
    d_rssi[tuple_idx] = sum / static_cast<float>(L_ra);
  }
}

__global__ void k_cbf16_to_cf_combine(float*               __restrict__ d_combined,
                                      const unsigned short* __restrict__ d_samples,
                                      unsigned             nof_rx_ports,
                                      unsigned             nof_symbols,
                                      unsigned             L_ra,
                                      unsigned             sym_stride_elems,
                                      unsigned             ps_stride_elems,
                                      bool                 combine_symbols)
{
  const unsigned i = blockIdx.x * blockDim.x + threadIdx.x;
  if (i >= L_ra) {
    return;
  }
  const unsigned i_port = blockIdx.y;
  if (i_port >= nof_rx_ports) {
    return;
  }

  if (combine_symbols) {
    float acc_re = 0.0F;
    float acc_im = 0.0F;
    for (unsigned i_sym = 0; i_sym < nof_symbols; ++i_sym) {
      unsigned              elem = i_port * ps_stride_elems + i_sym * sym_stride_elems + i;
      const unsigned short* p    = d_samples + elem * 2u;
      acc_re += bf16_to_float(p[0]);
      acc_im += bf16_to_float(p[1]);
    }
    const unsigned out_elem            = i_port * L_ra + i;
    d_combined[2 * out_elem + 0]       = acc_re;
    d_combined[2 * out_elem + 1]       = acc_im;
  } else {

    const unsigned i_sym  = blockIdx.z;
    if (i_sym >= nof_symbols) {
      return;
    }
    unsigned              src_elem = i_port * ps_stride_elems + i_sym * sym_stride_elems + i;
    const unsigned short* p        = d_samples + src_elem * 2u;
    const unsigned        batch_idx = i_port * nof_symbols + i_sym;
    const unsigned        out_elem  = batch_idx * L_ra + i;
    d_combined[2 * out_elem + 0]    = bf16_to_float(p[0]);
    d_combined[2 * out_elem + 1]    = bf16_to_float(p[1]);
  }
}

__global__ void k_prod_conj(float*       __restrict__ d_out,
                            const float* __restrict__ d_in,
                            const float* __restrict__ d_root,
                            unsigned     batch,
                            unsigned     L_ra,
                            unsigned     nof_sequences)
{
  const unsigned i = blockIdx.x * blockDim.x + threadIdx.x;
  if (i >= L_ra) {
    return;
  }
  const unsigned b = blockIdx.y;
  const unsigned s = blockIdx.z;
  if (b >= batch || s >= nof_sequences) {
    return;
  }
  const unsigned in_idx   = b * L_ra + i;
  const unsigned out_idx  = (s * batch + b) * L_ra + i;
  const float    a_re     = d_in[2 * in_idx + 0];
  const float    a_im     = d_in[2 * in_idx + 1];
  const float    r_re     = d_root[2 * (s * L_ra + i) + 0];
  const float    r_im     = d_root[2 * (s * L_ra + i) + 1];

  d_out[2 * out_idx + 0] = a_re * r_re + a_im * r_im;
  d_out[2 * out_idx + 1] = a_im * r_re - a_re * r_im;
}

__global__ void k_bin_reorder_idft_in(float*       __restrict__ d_idft_in,
                                      const float* __restrict__ d_no_root,
                                      unsigned     batch,
                                      unsigned     L_ra,
                                      unsigned     dft_size)
{
  const unsigned i = blockIdx.x * blockDim.x + threadIdx.x;
  if (i >= dft_size) {
    return;
  }
  const unsigned b = blockIdx.y;
  if (b >= batch) {
    return;
  }
  const unsigned half_lo = L_ra / 2u + 1u;
  const unsigned half_hi = L_ra / 2u;
  const unsigned dst_idx = b * dft_size + i;

  float re = 0.0F, im = 0.0F;
  if (i < half_lo) {
    unsigned src = b * L_ra + (L_ra - half_lo + i);
    re           = d_no_root[2 * src + 0];
    im           = d_no_root[2 * src + 1];
  } else if (i >= dft_size - half_hi) {
    unsigned src = b * L_ra + (i - (dft_size - half_hi));
    re           = d_no_root[2 * src + 0];
    im           = d_no_root[2 * src + 1];
  }
  d_idft_in[2 * dst_idx + 0] = re;
  d_idft_in[2 * dst_idx + 1] = im;
}

__global__ void k_modulus_square_scale(float*       __restrict__ d_mod_sq,
                                       const float* __restrict__ d_idft_out,
                                       unsigned     batch,
                                       unsigned     dft_size,
                                       float        scale)
{
  const unsigned i = blockIdx.x * blockDim.x + threadIdx.x;
  if (i >= dft_size) {
    return;
  }
  const unsigned b = blockIdx.y;
  if (b >= batch) {
    return;
  }
  const unsigned idx = b * dft_size + i;
  const float    re  = d_idft_out[2 * idx + 0];
  const float    im  = d_idft_out[2 * idx + 1];
  d_mod_sq[idx]      = (re * re + im * im) * scale;
}

__global__ void k_per_shift_accumulate(float*       __restrict__ d_num,
                                       float*       __restrict__ d_den,
                                       const float* __restrict__ d_mod_sq,
                                       unsigned     batch,
                                       unsigned     dft_size,
                                       unsigned     L_ra,
                                       unsigned     N_cs,
                                       unsigned     nof_shifts,
                                       unsigned     win_width,
                                       unsigned     win_margin,
                                       unsigned     nof_sequences)
{
  const unsigned i_window = blockIdx.x;
  const unsigned s        = blockIdx.y;
  if (i_window >= nof_shifts || s >= nof_sequences) {
    return;
  }
  const unsigned window_start = (dft_size - (N_cs * i_window * dft_size) / L_ra) % dft_size;
  const unsigned ref_start    = (window_start + dft_size - win_margin) % dft_size;
  const unsigned ref_total    = 2 * win_margin + win_width;

  constexpr unsigned MAX_BATCH = 64;
  __shared__ float   s_ref[MAX_BATCH];

  for (unsigned b = threadIdx.x; b < batch && b < MAX_BATCH; b += blockDim.x) {
    const unsigned mod_sq_base = (s * batch + b) * dft_size;
    float          ref         = 0.0F;
    for (unsigned k = 0; k < ref_total; ++k) {
      const unsigned idx = (ref_start + k) % dft_size;
      ref += d_mod_sq[mod_sq_base + idx];
    }
    s_ref[b] = ref;
  }
  __syncthreads();

  const float    scale    = static_cast<float>(dft_size) / static_cast<float>(L_ra);
  const unsigned num_base = (s * nof_shifts + i_window) * win_width;

  for (unsigned i = threadIdx.x; i < win_width; i += blockDim.x) {
    const unsigned mod_sq_idx = (window_start + i) % dft_size;
    float          num_acc    = 0.0F;
    float          den_acc    = 0.0F;
    for (unsigned b = 0; b < batch; ++b) {
      const unsigned mod_sq_base = (s * batch + b) * dft_size;
      const float    scaled      = d_mod_sq[mod_sq_base + mod_sq_idx] * scale;
      num_acc += scaled;
      den_acc += s_ref[b] - scaled;
    }
    d_num[num_base + i] = num_acc;
    d_den[num_base + i] = den_acc;
  }
}

__global__ void k_finalize_argmax(uint32_t*      __restrict__ d_delay,
                                  unsigned char* __restrict__ d_detected,
                                  float*         __restrict__ d_metric,
                                  float*         __restrict__ d_power,
                                  float*         __restrict__ d_num,
                                  float*         __restrict__ d_den,
                                  unsigned       nof_shifts,
                                  unsigned       win_width,
                                  unsigned       nof_sequences,
                                  float          threshold,
                                  float          detection_threshold_margin,
                                  unsigned       max_delay_samples,
                                  unsigned       power_normalization)
{
  const unsigned i_window = blockIdx.x;
  const unsigned s        = blockIdx.y;
  if (i_window >= nof_shifts || s >= nof_sequences) {
    return;
  }
  const unsigned flat = s * nof_shifts + i_window;
  const unsigned base = flat * win_width;

  __shared__ float    s_val[128];
  __shared__ uint32_t s_idx[128];

  float    local_max = -1.0F;
  uint32_t local_idx = 0;
  for (unsigned i = threadIdx.x; i < win_width; i += blockDim.x) {
    float den       = fabsf(d_den[base + i]);
    d_den[base + i] = den;

    float m = d_num[base + i] / den;
    if (m > local_max) {
      local_max = m;
      local_idx = i;
    }
  }
  s_val[threadIdx.x] = local_max;
  s_idx[threadIdx.x] = local_idx;
  __syncthreads();

  for (unsigned off = blockDim.x >> 1; off > 0; off >>= 1) {
    if (threadIdx.x < off) {
      if (s_val[threadIdx.x + off] > s_val[threadIdx.x]) {
        s_val[threadIdx.x] = s_val[threadIdx.x + off];
        s_idx[threadIdx.x] = s_idx[threadIdx.x + off];
      }
    }
    __syncthreads();
  }
  if (threadIdx.x == 0) {
    const float    peak  = s_val[0];
    const uint32_t delay = s_idx[0];
    const float    nad   = d_num[base + delay];

    const bool     det   = (delay < win_width) && (peak > threshold * detection_threshold_margin) &&
                       (static_cast<float>(delay) < static_cast<float>(max_delay_samples) * 0.8F);
    d_delay[flat]    = delay;
    d_detected[flat] = det ? 1u : 0u;
    d_metric[flat]   = peak / threshold;
    d_power[flat]    = nad / static_cast<float>(power_normalization);
  }
}

}

int launch_rssi_reduce(float*       d_rssi_per_tuple,
                       const void*  d_samples_base,
                       unsigned     nof_rx_ports,
                       unsigned     nof_symbols,
                       unsigned     L_ra,
                       unsigned     sym_stride_elems,
                       unsigned     ps_stride_elems,
                       void*        stream)
{
  if (nof_rx_ports == 0 || nof_symbols == 0 || L_ra == 0) {
    return 0;
  }
  const unsigned n_tuples = nof_rx_ports * nof_symbols;
  const unsigned threads  = 128;
  k_rssi_reduce<<<n_tuples, threads, 0, static_cast<cudaStream_t>(stream)>>>(
      d_rssi_per_tuple,
      static_cast<const unsigned short*>(d_samples_base),
      nof_rx_ports,
      nof_symbols,
      L_ra,
      sym_stride_elems,
      ps_stride_elems);
  return (cudaGetLastError() == cudaSuccess) ? 0 : -1;
}

int launch_cbf16_to_cf_combine(void*       d_combined,
                               const void* d_samples_base,
                               unsigned    nof_rx_ports,
                               unsigned    nof_symbols,
                               unsigned    L_ra,
                               unsigned    sym_stride_elems,
                               unsigned    ps_stride_elems,
                               bool        combine_symbols,
                               void*       stream)
{
  if (L_ra == 0 || nof_rx_ports == 0) {
    return 0;
  }
  const unsigned threads = 128;
  dim3           grid((L_ra + threads - 1) / threads, nof_rx_ports, combine_symbols ? 1 : nof_symbols);
  k_cbf16_to_cf_combine<<<grid, threads, 0, static_cast<cudaStream_t>(stream)>>>(
      static_cast<float*>(d_combined),
      static_cast<const unsigned short*>(d_samples_base),
      nof_rx_ports,
      nof_symbols,
      L_ra,
      sym_stride_elems,
      ps_stride_elems,
      combine_symbols);
  return (cudaGetLastError() == cudaSuccess) ? 0 : -1;
}

int launch_prod_conj(void*       d_out,
                     const void* d_in,
                     const void* d_root,
                     unsigned    batch,
                     unsigned    L_ra,
                     unsigned    nof_sequences,
                     void*       stream)
{
  if (batch == 0 || L_ra == 0 || nof_sequences == 0) {
    return 0;
  }
  const unsigned threads = 128;
  dim3           grid((L_ra + threads - 1) / threads, batch, nof_sequences);
  k_prod_conj<<<grid, threads, 0, static_cast<cudaStream_t>(stream)>>>(
      static_cast<float*>(d_out),
      static_cast<const float*>(d_in),
      static_cast<const float*>(d_root),
      batch,
      L_ra,
      nof_sequences);
  return (cudaGetLastError() == cudaSuccess) ? 0 : -1;
}

int launch_bin_reorder(void*       d_idft_in,
                       const void* d_no_root,
                       unsigned    batch,
                       unsigned    L_ra,
                       unsigned    dft_size,
                       void*       stream)
{
  if (batch == 0 || dft_size == 0) {
    return 0;
  }
  const unsigned threads = 256;
  dim3           grid((dft_size + threads - 1) / threads, batch, 1);
  k_bin_reorder_idft_in<<<grid, threads, 0, static_cast<cudaStream_t>(stream)>>>(
      static_cast<float*>(d_idft_in),
      static_cast<const float*>(d_no_root),
      batch,
      L_ra,
      dft_size);
  return (cudaGetLastError() == cudaSuccess) ? 0 : -1;
}

int launch_modulus_square_scale(float*       d_mod_sq,
                                const void*  d_idft_out,
                                unsigned     batch,
                                unsigned     dft_size,
                                float        scale,
                                void*        stream)
{
  if (batch == 0 || dft_size == 0) {
    return 0;
  }
  const unsigned threads = 256;
  dim3           grid((dft_size + threads - 1) / threads, batch, 1);
  k_modulus_square_scale<<<grid, threads, 0, static_cast<cudaStream_t>(stream)>>>(
      d_mod_sq, static_cast<const float*>(d_idft_out), batch, dft_size, scale);
  return (cudaGetLastError() == cudaSuccess) ? 0 : -1;
}

int launch_per_shift_accumulate(float*       d_num,
                                float*       d_den,
                                const float* d_mod_sq,
                                unsigned     batch,
                                unsigned     dft_size,
                                unsigned     L_ra,
                                unsigned     N_cs,
                                unsigned     nof_shifts,
                                unsigned     win_width,
                                unsigned     win_margin,
                                unsigned     nof_sequences,
                                void*        stream)
{
  if (nof_shifts == 0 || batch == 0 || win_width == 0 || nof_sequences == 0) {
    return 0;
  }
  const unsigned threads = 128;

  dim3           grid(nof_shifts, nof_sequences, 1);
  k_per_shift_accumulate<<<grid, threads, 0, static_cast<cudaStream_t>(stream)>>>(
      d_num, d_den, d_mod_sq, batch, dft_size, L_ra, N_cs, nof_shifts, win_width, win_margin, nof_sequences);
  return (cudaGetLastError() == cudaSuccess) ? 0 : -1;
}

int launch_finalize_argmax(uint32_t*      d_delay,
                           unsigned char* d_detected,
                           float*         d_metric,
                           float*         d_power,
                           float*         d_num,
                           float*         d_den,
                           unsigned       nof_shifts,
                           unsigned       win_width,
                           unsigned       nof_sequences,
                           float          threshold,
                           float          detection_threshold_margin,
                           unsigned       max_delay_samples,
                           unsigned       power_normalization,
                           void*          stream)
{
  if (nof_shifts == 0 || win_width == 0 || nof_sequences == 0) {
    return 0;
  }
  dim3 grid(nof_shifts, nof_sequences, 1);
  k_finalize_argmax<<<grid, 128, 0, static_cast<cudaStream_t>(stream)>>>(
      d_delay, d_detected, d_metric, d_power, d_num, d_den, nof_shifts, win_width, nof_sequences,
      threshold, detection_threshold_margin, max_delay_samples, power_normalization);
  return (cudaGetLastError() == cudaSuccess) ? 0 : -1;
}

}
}
}
