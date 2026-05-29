#pragma once

#include <cstdint>

namespace ocudu {
namespace hal {
namespace cuda {

int launch_rssi_reduce(float*       d_rssi_per_tuple,
                       const void*  d_samples_base,
                       unsigned     nof_rx_ports,
                       unsigned     nof_symbols,
                       unsigned     L_ra,
                       unsigned     sym_stride_elems,
                       unsigned     ps_stride_elems,
                       void*        stream);

int launch_cbf16_to_cf_combine(void*       d_combined,
                               const void* d_samples_base,
                               unsigned    nof_rx_ports,
                               unsigned    nof_symbols,
                               unsigned    L_ra,
                               unsigned    sym_stride_elems,
                               unsigned    ps_stride_elems,
                               bool        combine_symbols,
                               void*       stream);

int launch_prod_conj(void*       d_out,
                     const void* d_in,
                     const void* d_root,
                     unsigned    batch,
                     unsigned    L_ra,
                     unsigned    nof_sequences,
                     void*       stream);

int launch_bin_reorder(void*       d_idft_in,
                       const void* d_no_root,
                       unsigned    batch,
                       unsigned    L_ra,
                       unsigned    dft_size,
                       void*       stream);

int launch_modulus_square_scale(float*       d_mod_sq,
                                const void*  d_idft_out,
                                unsigned     batch,
                                unsigned     dft_size,
                                float        scale,
                                void*        stream);

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
                                void*        stream);

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
                           void*          stream);

}
}
}
