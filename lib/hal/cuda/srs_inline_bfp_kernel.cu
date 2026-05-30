#include "ocudu/hal/cuda/srs_inline_bfp_kernel.h"

#include <algorithm>
#include <cstdio>
#include <cuda_runtime.h>

namespace ocudu {
namespace hal {
namespace cuda {

namespace {

__device__ __forceinline__ constexpr unsigned RE_PER_PRB() { return 12; }

__device__ __forceinline__ int extract_packed_sample(const unsigned char* mantissa_bytes,
                                                     unsigned             bit_pos,
                                                     unsigned             data_width)
{
  const unsigned byte_pos    = bit_pos >> 3;
  const unsigned bit_in_byte = bit_pos & 7u;
  const unsigned b0          = mantissa_bytes[byte_pos];
  const unsigned b1          = mantissa_bytes[byte_pos + 1];

  const unsigned b2       = (data_width + bit_in_byte > 16) ? mantissa_bytes[byte_pos + 2] : 0u;
  const unsigned combined = (b0 << 16) | (b1 << 8) | b2;
  const unsigned shift    = 24u - bit_in_byte - data_width;
  const unsigned mask     = (1u << data_width) - 1u;
  const unsigned raw      = (combined >> shift) & mask;
  const unsigned sign_bit = 1u << (data_width - 1);
  return static_cast<int>(raw ^ sign_bit) - static_cast<int>(sign_bit);
}

__global__ void k_bfp_decompress_srs_re_extract(const srs_packet_descriptor* __restrict__ descs,
                                                unsigned prb_bytes,
                                                unsigned nof_prbs,
                                                unsigned data_width,
                                                unsigned mapping_initial_subcarrier,
                                                unsigned comb_offset,
                                                unsigned comb_size,
                                                unsigned sequence_length,
                                                float    quantizer_gain)
{
  const srs_packet_descriptor& d        = descs[blockIdx.x];
  const unsigned               total_re = nof_prbs * RE_PER_PRB();

  for (unsigned re_idx_global = threadIdx.x; re_idx_global < total_re; re_idx_global += blockDim.x) {
    const unsigned prb_idx   = re_idx_global / RE_PER_PRB();
    const unsigned re_in_prb = re_idx_global % RE_PER_PRB();

    if (re_idx_global < mapping_initial_subcarrier + comb_offset) {
      continue;
    }
    const unsigned re_offset = re_idx_global - mapping_initial_subcarrier - comb_offset;
    if ((re_offset % comb_size) != 0u) {
      continue;
    }
    const unsigned srs_idx = re_offset / comb_size;
    if (srs_idx >= sequence_length) {
      continue;
    }

    const unsigned char* prb_base = reinterpret_cast<const unsigned char*>(d.compressed_bytes) +
                                    static_cast<size_t>(prb_idx) * prb_bytes;
    const unsigned       exponent = prb_base[0];
    const unsigned char* mantissa = prb_base + 1;

    const unsigned sample_idx_re_im = re_in_prb * 2u;
    const unsigned bit_pos_re       = sample_idx_re_im * data_width;
    const unsigned bit_pos_im       = bit_pos_re + data_width;

    const int re_raw = extract_packed_sample(mantissa, bit_pos_re, data_width);
    const int im_raw = extract_packed_sample(mantissa, bit_pos_im, data_width);

    const float scaler = static_cast<float>(1u << exponent);
    const float re_val = (static_cast<float>(re_raw) * scaler) / quantizer_gain;
    const float im_val = (static_cast<float>(im_raw) * scaler) / quantizer_gain;

    float2* out = reinterpret_cast<float2*>(d.out_sequence) + srs_idx;
    out->x      = re_val;
    out->y      = im_val;
  }
}

}

int launch_bfp_decompress_srs_re_extract(const srs_packet_descriptor* descs,
                                         srs_packet_descriptor*       d_descs_scratch,
                                         unsigned                     n_packets,
                                         unsigned                     prb_bytes,
                                         unsigned                     nof_prbs,
                                         unsigned                     data_width,
                                         unsigned                     mapping_initial_subcarrier,
                                         unsigned                     comb_offset,
                                         unsigned                     comb_size,
                                         unsigned                     sequence_length,
                                         float                        quantizer_gain,
                                         void*                        stream)
{
  if (n_packets == 0) {
    return 0;
  }
  if (comb_size == 0u) {
    std::fprintf(stderr, "[srs_inline_bfp] invalid comb_size=0\n");
    return -1;
  }

  const unsigned threads_per_block = std::min(((nof_prbs * 12u) + 31u) & ~31u, 1024u);

  const size_t bytes = static_cast<size_t>(n_packets) * sizeof(srs_packet_descriptor);
  if (cudaError_t e = cudaMemcpyAsync(d_descs_scratch,
                                      descs,
                                      bytes,
                                      cudaMemcpyHostToDevice,
                                      static_cast<cudaStream_t>(stream));
      e != cudaSuccess) {
    std::fprintf(stderr,
                 "[srs_inline_bfp] cudaMemcpyAsync(descs) failed: %s\n",
                 cudaGetErrorString(e));
    return -2;
  }

  k_bfp_decompress_srs_re_extract<<<n_packets, threads_per_block, 0, static_cast<cudaStream_t>(stream)>>>(
      d_descs_scratch,
      prb_bytes,
      nof_prbs,
      data_width,
      mapping_initial_subcarrier,
      comb_offset,
      comb_size,
      sequence_length,
      quantizer_gain);

  if (cudaError_t e = cudaGetLastError(); e != cudaSuccess) {
    std::fprintf(stderr,
                 "[srs_inline_bfp] kernel launch failed: %s (n=%u thr=%u)\n",
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
