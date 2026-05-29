#include "ocudu/hal/cuda/prach_inline_bfp_kernel.h"

#include <cstdio>
#include <cuda_runtime.h>

namespace ocudu {
namespace hal {
namespace cuda {

namespace {

__device__ __forceinline__ constexpr unsigned RE_PER_PRB() { return 12; }

__device__ __forceinline__ unsigned short float_to_bf16(float f)
{
  unsigned int bits;
  static_assert(sizeof(bits) == sizeof(f), "expected 32-bit float");
  bits           = __float_as_uint(f);

  unsigned int rounding = ((bits >> 16) & 1u) + 0x7FFFu;
  return static_cast<unsigned short>((bits + rounding) >> 16);
}

__device__ __forceinline__ int extract_packed_sample(const unsigned char* mantissa_bytes,
                                                     unsigned             bit_pos,
                                                     unsigned             data_width)
{
  const unsigned byte_pos          = bit_pos >> 3;
  const unsigned bit_in_byte       = bit_pos & 7u;

  const unsigned b0 = mantissa_bytes[byte_pos];
  const unsigned b1 = mantissa_bytes[byte_pos + 1];

  const unsigned b2 = (data_width + bit_in_byte > 16) ? mantissa_bytes[byte_pos + 2] : 0u;
  const unsigned combined = (b0 << 16) | (b1 << 8) | b2;

  const unsigned shift = 24u - bit_in_byte - data_width;
  const unsigned mask  = (1u << data_width) - 1u;
  const unsigned raw   = (combined >> shift) & mask;

  const unsigned sign_bit = 1u << (data_width - 1);
  return static_cast<int>(raw ^ sign_bit) - static_cast<int>(sign_bit);
}

__global__ void k_bfp_decompress_re_demap(const prach_packet_descriptor* __restrict__ descs,
                                          unsigned prb_bytes,
                                          unsigned nof_prbs,
                                          unsigned data_width,
                                          unsigned k_bar,
                                          unsigned L_ra,
                                          float    quantizer_gain)
{
  const unsigned re_idx_global = threadIdx.x;
  const unsigned prb_idx       = re_idx_global / RE_PER_PRB();
  const unsigned re_in_prb     = re_idx_global % RE_PER_PRB();

  if (prb_idx >= nof_prbs) {
    return;
  }

  const prach_packet_descriptor& d = descs[blockIdx.x];

  const unsigned char* prb_base   = reinterpret_cast<const unsigned char*>(d.compressed_bytes) +
                                   static_cast<size_t>(prb_idx) * prb_bytes;
  const unsigned       exponent   = prb_base[0];
  const unsigned char* mantissa   = prb_base + 1;

  const unsigned sample_idx_re_im = re_in_prb * 2u;
  const unsigned bit_pos_re       = sample_idx_re_im * data_width;
  const unsigned bit_pos_im       = bit_pos_re + data_width;

  const int re_raw = extract_packed_sample(mantissa, bit_pos_re, data_width);
  const int im_raw = extract_packed_sample(mantissa, bit_pos_im, data_width);

  const float scaler  = static_cast<float>(1u << exponent);
  const float re_val  = (static_cast<float>(re_raw) * scaler) / quantizer_gain;
  const float im_val  = (static_cast<float>(im_raw) * scaler) / quantizer_gain;

  if (re_idx_global < k_bar) {
    return;
  }
  const unsigned out_idx = re_idx_global - k_bar;
  if (out_idx >= L_ra) {
    return;
  }

  unsigned short* out = reinterpret_cast<unsigned short*>(d.out_sequence) + (out_idx * 2u);
  out[0]              = float_to_bf16(re_val);
  out[1]              = float_to_bf16(im_val);
}

}

int launch_bfp_decompress_re_demap(const prach_packet_descriptor* descs,
                                   prach_packet_descriptor*       d_descs_scratch,
                                   unsigned                       n_packets,
                                   unsigned                       prb_bytes,
                                   unsigned                       nof_prbs,
                                   unsigned                       data_width,
                                   unsigned                       k_bar,
                                   unsigned                       L_ra,
                                   float                          quantizer_gain,
                                   void*                          stream)
{
  if (n_packets == 0) {
    return 0;
  }

  const unsigned threads_per_block = ((nof_prbs * 12u) + 31u) & ~31u;

  const size_t bytes = static_cast<size_t>(n_packets) * sizeof(prach_packet_descriptor);
  if (cudaError_t e = cudaMemcpyAsync(d_descs_scratch,
                                       descs,
                                       bytes,
                                       cudaMemcpyHostToDevice,
                                       static_cast<cudaStream_t>(stream));
      e != cudaSuccess) {
    std::fprintf(stderr,
                 "[prach_inline_bfp] cudaMemcpyAsync(descs) failed: %s\n",
                 cudaGetErrorString(e));
    return -2;
  }

  k_bfp_decompress_re_demap<<<n_packets, threads_per_block, 0, static_cast<cudaStream_t>(stream)>>>(
      d_descs_scratch, prb_bytes, nof_prbs, data_width, k_bar, L_ra, quantizer_gain);

  if (cudaError_t e = cudaGetLastError(); e != cudaSuccess) {
    std::fprintf(stderr,
                 "[prach_inline_bfp] kernel launch failed: %s (n=%u thr=%u)\n",
                 cudaGetErrorString(e),
                 n_packets,
                 threads_per_block);
    return -3;
  }
  return 0;
}

}
}
}
