#include "ocudu/hal/cuda/inline_srs_pipeline.h"

#include "ocudu/ran/srs/srs_information.h"
#include "fmt/format.h"

#include <algorithm>
#include <cstdio>
#include <cuda_runtime.h>
#include <vector>

namespace ocudu {
namespace hal {
namespace cuda {

namespace {

struct slot_bucket {
  bool                               in_use          = false;
  uint32_t                           slot_id         = 0;
  unsigned                           arrived         = 0;
  unsigned                           expected        = 0;
  unsigned                           nof_symbols     = 0;
  unsigned                           start_symbol    = 0;
  unsigned                           nof_prbs        = 0;
  unsigned                           start_prb       = 0;
  srs_resource_configuration         resource{};
  std::vector<srs_packet_descriptor> descs;
  std::vector<unsigned char>         desc_filled;
  std::vector<std::function<void()>> mbuf_release_fns;
};

class inline_srs_pipeline_impl : public inline_srs_pipeline
{
public:
  explicit inline_srs_pipeline_impl(const config& cfg) : cfg_(cfg)
  {
    max_per_occasion_ = cfg_.max_nof_rx_ports * cfg_.max_nof_symbols;
    buckets_.resize(std::max(1u, cfg_.slot_ring_depth));
    for (auto& b : buckets_) {
      b.descs.assign(max_per_occasion_, srs_packet_descriptor{});
      b.desc_filled.assign(max_per_occasion_, 0);
      b.mbuf_release_fns.reserve(max_per_occasion_);
    }
    cudaStreamCreateWithFlags(reinterpret_cast<cudaStream_t*>(&stream_), cudaStreamNonBlocking);
  }

  ~inline_srs_pipeline_impl() override
  {
    if (d_descs_ != nullptr) {
      cudaFree(d_descs_);
    }
    if (stream_ != nullptr) {
      cudaStreamDestroy(static_cast<cudaStream_t>(stream_));
    }
    for (auto& b : buckets_) {
      for (auto& fn : b.mbuf_release_fns) {
        if (fn) {
          fn();
        }
      }
    }
  }

  bool init()
  {

    vram_srs_buffer::config vc{cfg_.max_nof_rx_ports, cfg_.max_nof_symbols, cfg_.max_sequence_length};
    vram_buffer_ = vram_srs_buffer::create(vc);
    if (!vram_buffer_) {
      std::fprintf(stderr, "[inline_srs_pipeline] vram_srs_buffer::create failed\n");
      return false;
    }

    srs_estimator_inline::config gcfg{};
    gcfg.max_nof_rx_ports      = cfg_.max_nof_rx_ports;
    gcfg.max_nof_antenna_ports = 1;
    gcfg.max_nof_symbols       = cfg_.max_nof_symbols;
    gcfg.max_sequence_length   = cfg_.max_sequence_length;
    gcfg.max_batch             = 1;
    estimator_                 = srs_estimator_inline::create(gcfg);
    if (!estimator_) {
      std::fprintf(stderr, "[inline_srs_pipeline] srs_estimator_inline::create failed\n");
      return false;
    }

    if (cudaError_t e = cudaMalloc(&d_descs_, max_per_occasion_ * sizeof(srs_packet_descriptor));
        e != cudaSuccess) {
      std::fprintf(stderr, "[inline_srs_pipeline] cudaMalloc(d_descs) failed: %s\n", cudaGetErrorString(e));
      return false;
    }

    return true;
  }

  void on_packet(uint32_t                          system_slot_id,
                 unsigned                          symbol_idx,
                 unsigned                          rx_port_idx,
                 unsigned                          nof_prbs,
                 unsigned                          start_prb,
                 const srs_resource_configuration& resource,
                 unsigned                          nof_rx_ports,
                 const void*                       d_compressed_bytes,
                 std::function<void()>             mbuf_release) override
  {
    const unsigned start_symbol = resource.start_symbol.value();
    const unsigned nof_symbols  = static_cast<unsigned>(resource.nof_symbols);
    if (symbol_idx < start_symbol || rx_port_idx >= nof_rx_ports || nof_rx_ports > cfg_.max_nof_rx_ports ||
        nof_symbols == 0 || nof_symbols > cfg_.max_nof_symbols) {
      if (mbuf_release) {
        mbuf_release();
      }
      return;
    }
    const unsigned sym_off = symbol_idx - start_symbol;
    if (sym_off >= nof_symbols) {
      if (mbuf_release) {
        mbuf_release();
      }
      return;
    }

    slot_bucket& b = find_or_alloc_bucket(system_slot_id);
    if (b.arrived == 0) {

      b.resource     = resource;
      b.nof_symbols  = nof_symbols;
      b.start_symbol = start_symbol;

      b.nof_prbs     = (nof_prbs == 0) ? cfg_.cell_nof_prbs : nof_prbs;
      b.start_prb    = start_prb;
      b.expected     = nof_rx_ports * nof_symbols;
    }

    const unsigned desc_idx = rx_port_idx * nof_symbols + sym_off;
    if (desc_idx >= max_per_occasion_ || b.desc_filled[desc_idx]) {
      if (mbuf_release) {
        mbuf_release();
      }
      return;
    }

    srs_packet_descriptor d{};
    d.compressed_bytes      = static_cast<const uint8_t*>(d_compressed_bytes);
    d.out_sequence          = vram_buffer_->device_ptr_for_symbol(rx_port_idx, sym_off);
    b.descs[desc_idx]       = d;
    b.desc_filled[desc_idx] = 1;
    b.mbuf_release_fns.emplace_back(std::move(mbuf_release));
    ++b.arrived;

    if (b.arrived == b.expected) {
      fire_occasion(b);
    }
  }

  void flush_stale_slots(uint32_t now_slot_id, unsigned keepalive_slots) override
  {
    for (auto& b : buckets_) {
      if (!b.in_use) {
        continue;
      }
      const uint32_t age = now_slot_id - b.slot_id;
      if (age > keepalive_slots) {
        std::fprintf(stderr,
                     "[inline_srs_pipeline] WARN: slot evicted: id=%u age=%u arrived=%u/%u\n",
                     b.slot_id,
                     age,
                     b.arrived,
                     b.expected);
        for (auto& fn : b.mbuf_release_fns) {
          if (fn) {
            fn();
          }
        }
        reset_bucket(b);
      }
    }
  }

private:
  slot_bucket& find_or_alloc_bucket(uint32_t slot_id)
  {
    for (auto& b : buckets_) {
      if (b.in_use && b.slot_id == slot_id) {
        return b;
      }
    }
    for (auto& b : buckets_) {
      if (!b.in_use) {
        b.in_use  = true;
        b.slot_id = slot_id;
        b.arrived = 0;
        std::fill(b.desc_filled.begin(), b.desc_filled.end(), 0);
        b.mbuf_release_fns.clear();
        return b;
      }
    }
    slot_bucket& victim = buckets_.front();
    std::fprintf(stderr,
                 "[inline_srs_pipeline] WARN: bucket exhaustion, evicting slot=%u (arrived=%u/%u) "
                 "for incoming slot=%u\n",
                 victim.slot_id,
                 victim.arrived,
                 victim.expected,
                 slot_id);
    for (auto& fn : victim.mbuf_release_fns) {
      if (fn) {
        fn();
      }
    }
    reset_bucket(victim);
    victim.in_use  = true;
    victim.slot_id = slot_id;
    return victim;
  }

  void reset_bucket(slot_bucket& b)
  {
    b.in_use          = false;
    b.slot_id         = 0;
    b.arrived         = 0;
    b.expected        = 0;
    std::fill(b.desc_filled.begin(), b.desc_filled.end(), 0);
    b.mbuf_release_fns.clear();
  }

  void fire_occasion(slot_bucket& b)
  {
    cudaStream_t s = static_cast<cudaStream_t>(stream_);

    const srs_information info       = get_srs_information(b.resource, 0);
    const unsigned        comb_size  = info.comb_size;
    const unsigned        seq_len    = info.sequence_length;

    const unsigned        mapping_abs   = info.mapping_initial_subcarrier;
    const unsigned        packet_re0    = b.start_prb * 12u;
    const unsigned        mapping_local = (mapping_abs >= packet_re0) ? (mapping_abs - packet_re0) : 0u;

    if (seq_len == 0 || seq_len > cfg_.max_sequence_length) {
      std::fprintf(stderr, "[inline_srs_pipeline] occasion seq_len=%u exceeds max=%u; dropping slot=%u\n",
                   seq_len, cfg_.max_sequence_length, b.slot_id);
      for (auto& fn : b.mbuf_release_fns) {
        if (fn) {
          fn();
        }
      }
      reset_bucket(b);
      return;
    }

    int rc = launch_bfp_decompress_srs_re_extract(b.descs.data(),
                                                  static_cast<srs_packet_descriptor*>(d_descs_),
                                                  b.expected,
                                                  cfg_.prb_bytes,
                                                  b.nof_prbs,
                                                  cfg_.data_width,
                                                  mapping_local,
                                                  0,
                                                  comb_size,
                                                  seq_len,
                                                  cfg_.quantizer_gain,
                                                  s);
    if (rc != 0) {
      std::fprintf(stderr, "[inline_srs_pipeline] BFP launch rc=%d slot=%u\n", rc, b.slot_id);
    }

    cudaStreamSynchronize(s);

    srs_estimator_configuration ecfg{};
    ecfg.slot     = reconstruct_slot(b.slot_id);
    ecfg.resource = b.resource;
    const unsigned nof_rx = b.expected / b.nof_symbols;
    ecfg.ports.resize(nof_rx);
    for (unsigned r = 0; r < nof_rx; ++r) {
      ecfg.ports[r] = static_cast<uint8_t>(r);
    }
    srs_estimator_result result = estimator_->estimate_inline(*vram_buffer_, ecfg);

    for (auto& fn : b.mbuf_release_fns) {
      if (fn) {
        fn();
      }
    }

    const float norm = result.channel_matrix.frobenius_norm();
    fmt::print(stderr,
               "[srs_pipeline backend=gpu] slot={} epre={:.2f}dB rsrp={:.2f}dB ch_norm2={:.4f} "
               "noise_var={:.3e} ta={}ns\n",
               b.slot_id,
               result.epre_dB.value_or(0.0F),
               result.rsrp_dB.value_or(0.0F),
               norm * norm,
               result.noise_variance.value_or(0.0F),
               static_cast<long>(result.time_alignment.time_alignment * 1e9));

    if (cfg_.on_result_cb) {
      cfg_.on_result_cb(result, b.slot_id);
    }

    reset_bucket(b);
  }

  slot_point reconstruct_slot(uint32_t packed) const
  {
    const unsigned sfn      = (packed >> 16) & 0xFFu;
    const unsigned subframe = (packed >> 8) & 0xFFu;
    const unsigned slot_sf  = packed & 0xFFu;
    const unsigned slots_sf = 1u << cfg_.numerology;
    return slot_point(cfg_.numerology, sfn, subframe, std::min(slot_sf, slots_sf - 1));
  }

  config                                cfg_;
  unsigned                              max_per_occasion_ = 0;
  std::vector<slot_bucket>              buckets_;
  std::unique_ptr<vram_srs_buffer>      vram_buffer_;
  std::unique_ptr<srs_estimator_inline> estimator_;
  void*                                 stream_  = nullptr;
  void*                                 d_descs_ = nullptr;
};

}

std::unique_ptr<inline_srs_pipeline> inline_srs_pipeline::create(const config& cfg)
{
  std::unique_ptr<inline_srs_pipeline_impl> p{new inline_srs_pipeline_impl(cfg)};
  if (!p->init()) {
    return nullptr;
  }
  return p;
}

}
}
}
