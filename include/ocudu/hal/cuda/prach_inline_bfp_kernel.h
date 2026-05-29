#pragma once

#include <cstddef>
#include <cstdint>

namespace ocudu {
namespace hal {
namespace cuda {

struct prach_packet_descriptor {
  const uint8_t* compressed_bytes;
  void*          out_sequence;
};

int launch_bfp_decompress_re_demap(const prach_packet_descriptor* descs,
                                   prach_packet_descriptor*       d_descs_scratch,
                                   unsigned                       n_packets,
                                   unsigned                       prb_bytes,
                                   unsigned                       nof_prbs,
                                   unsigned                       data_width,
                                   unsigned                       k_bar,
                                   unsigned                       L_ra,
                                   float                          quantizer_gain,
                                   void*                          stream);

}
}
}
