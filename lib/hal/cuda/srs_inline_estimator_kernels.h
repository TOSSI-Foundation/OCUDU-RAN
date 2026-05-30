#pragma once

#include <cstdint>

namespace ocudu {
namespace hal {
namespace cuda {

struct srs_occ_desc {
  const void* vram_base;
  unsigned    vram_port_stride;
  unsigned    vram_sym_stride;
  unsigned    nof_rx;
  unsigned    nof_tx;
  unsigned    nof_symbols;
  unsigned    seq_len;
  unsigned    num_groups;
  unsigned    dft_size;
  unsigned    max_ta_samples;
  unsigned    comb_size;
  unsigned    compute_frac;
  float       inv_sampling_rate;
  uint8_t     port_map[4];
  int8_t      noise_group[4];
  float       ratio_per_tx[4];
  float       ratio_per_group[2];
};

int launch_srs_lse_noise_accum_batch(void*       d_lse,
                                     void*       d_noise,
                                     float*      d_epre,
                                     const void* d_descs,
                                     unsigned    nof_occasions,
                                     unsigned    max_rx,
                                     unsigned    max_tx,
                                     unsigned    max_seq,
                                     void*       stream);

int launch_srs_prod_conj_scale_batch(void*       d_lse,
                                     const void* d_zc,
                                     const void* d_descs,
                                     unsigned    nof_occasions,
                                     unsigned    max_rx,
                                     unsigned    max_tx,
                                     unsigned    max_seq,
                                     void*       stream);

int launch_srs_ta_pack_idft_input_batch(void*       d_idft_dense,
                                        const void* d_lse,
                                        const void* d_descs,
                                        unsigned    tx_port,
                                        unsigned    nof_occasions,
                                        unsigned    max_rx,
                                        unsigned    max_tx,
                                        unsigned    max_seq,
                                        unsigned    batch_rx,
                                        unsigned    batch_dft,
                                        void*       stream);

int launch_srs_ta_modsq_accum_batch(float*      d_corr,
                                    const void* d_idft_dense,
                                    const void* d_descs,
                                    unsigned    nof_occasions,
                                    unsigned    max_dft,
                                    unsigned    batch_rx,
                                    unsigned    batch_dft,
                                    void*       stream);

int launch_srs_ta_extract_batch(const float* d_corr,
                                const void*  d_descs,
                                float*       d_psc,
                                float*       d_phtx,
                                float*       d_phgrp,
                                float*       d_ta,
                                unsigned     nof_occasions,
                                unsigned     max_tx,
                                unsigned     max_dft,
                                void*        stream);

int launch_srs_phase_compensate_batch(void*        d_lse,
                                      const float* d_phtx,
                                      const float* d_psc,
                                      const void*  d_descs,
                                      unsigned     nof_occasions,
                                      unsigned     max_rx,
                                      unsigned     max_tx,
                                      unsigned     max_seq,
                                      void*        stream);

int launch_srs_phase_compensate_noise_batch(void*        d_noise,
                                            const float* d_phgrp,
                                            const float* d_psc,
                                            const void*  d_descs,
                                            unsigned     nof_occasions,
                                            unsigned     max_rx,
                                            unsigned     max_seq,
                                            void*        stream);

int launch_srs_wideband_coeff_batch(void*       d_coeffs,
                                    const void* d_lse,
                                    const void* d_descs,
                                    unsigned    nof_occasions,
                                    unsigned    max_rx,
                                    unsigned    max_tx,
                                    unsigned    max_seq,
                                    void*       stream);

int launch_srs_signal_subtract_and_noise_batch(void*       d_noise,
                                               float*      d_nsq,
                                               const void* d_zc,
                                               const void* d_coeffs,
                                               const void* d_descs,
                                               unsigned    nof_occasions,
                                               unsigned    max_rx,
                                               unsigned    max_tx,
                                               unsigned    max_seq,
                                               void*       stream);

}
}
}
