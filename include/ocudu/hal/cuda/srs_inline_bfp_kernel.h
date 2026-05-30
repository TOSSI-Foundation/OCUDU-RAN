#pragma once

#include <cstddef>
#include <cstdint>

namespace ocudu {
namespace hal {
namespace cuda {

struct srs_packet_descriptor {
  const uint8_t* compressed_bytes;
  void*          out_sequence;
};

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
                                         void*                        stream);

}
}
}
