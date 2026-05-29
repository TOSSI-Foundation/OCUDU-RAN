#include "ocudu/hal/cuda/inline_prach_pipeline.h"

#include "fmt/format.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <cuda_runtime.h>
#include <vector>

namespace ocudu {
namespace hal {
namespace cuda {

namespace {

struct slot_bucket {
  bool                                 in_use     = false;
  uint32_t                             slot_id    = 0;
  unsigned                             arrived    = 0;
  std::vector<prach_packet_descriptor> descs;
  std::vector<unsigned char>           desc_filled;
  std::vector<std::function<void()>>   mbuf_release_fns;
};

class inline_prach_pipeline_impl : public inline_prach_pipeline
{
public:
  explicit inline_prach_pipeline_impl(const config& cfg) : cfg_(cfg)
  {
    expected_per_occasion_ = cfg_.nof_prach_eaxc * cfg_.nof_prach_symbols;
    buckets_.resize(std::max(1u, cfg_.slot_ring_depth));
    for (auto& b : buckets_) {
      b.descs.assign(expected_per_occasion_, prach_packet_descriptor{});
      b.desc_filled.assign(expected_per_occasion_, 0);
      b.mbuf_release_fns.clear();
      b.mbuf_release_fns.reserve(expected_per_occasion_);
    }
    cudaStreamCreateWithFlags(reinterpret_cast<cudaStream_t*>(&stream_), cudaStreamNonBlocking);
  }

  ~inline_prach_pipeline_impl() override
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

    vram_prach_buffer::config vc{};
    vc.nof_ports        = cfg_.nof_prach_eaxc;
    vc.nof_td_occasions = 1;
    vc.nof_fd_occasions = 1;
    vc.nof_symbols      = cfg_.nof_prach_symbols;
    vc.sequence_length  = cfg_.L_ra;
    vram_buffer_         = vram_prach_buffer::create(vc);
    if (!vram_buffer_) {
      std::fprintf(stderr, "[inline_prach_pipeline] vram_prach_buffer::create failed\n");
      return false;
    }
    detector_ = create_prach_detector_inline(cfg_.detector_inline_cfg);
    if (!detector_) {
      std::fprintf(stderr, "[inline_prach_pipeline] create_prach_detector_inline failed\n");
      return false;
    }

    if (cudaError_t e = cudaMalloc(&d_descs_, expected_per_occasion_ * sizeof(prach_packet_descriptor));
        e != cudaSuccess) {
      std::fprintf(stderr, "[inline_prach_pipeline] cudaMalloc(d_descs) failed: %s\n", cudaGetErrorString(e));
      return false;
    }
    detect_cfg_ = cfg_.detector_cfg;

    for (unsigned warm = 0; warm < 2; ++warm) {
      detector_->detect_inline(*vram_buffer_, detect_cfg_);
    }
    fmt::print(stderr,
               "[inline_prach_pipeline] warmup complete (ports={}, format={}, zcz={})\n",
               detect_cfg_.nof_rx_ports,
               static_cast<int>(detect_cfg_.format),
               detect_cfg_.zero_correlation_zone);
    return true;
  }

  void on_packet(uint32_t                  system_slot_id,
                 unsigned                  symbol_idx,
                 unsigned                  eaxc_port_idx,
                 const void*               d_compressed_bytes,
                 std::function<void()>     mbuf_release) override
  {
    if (eaxc_port_idx >= cfg_.nof_prach_eaxc || symbol_idx >= cfg_.nof_prach_symbols) {
      if (mbuf_release) {
        mbuf_release();
      }
      return;
    }
    slot_bucket& b = find_or_alloc_bucket(system_slot_id);

    const unsigned desc_idx =
        eaxc_port_idx * cfg_.nof_prach_symbols + symbol_idx;
    if (b.desc_filled[desc_idx]) {

      if (mbuf_release) {
        mbuf_release();
      }
      return;
    }

    prach_packet_descriptor d{};
    d.compressed_bytes = static_cast<const uint8_t*>(d_compressed_bytes);
    d.out_sequence     = vram_buffer_->device_ptr_for_symbol(eaxc_port_idx, 0, 0, symbol_idx);
    b.descs[desc_idx]  = d;
    b.desc_filled[desc_idx] = 1;
    b.mbuf_release_fns.emplace_back(std::move(mbuf_release));
    ++b.arrived;

    if (b.arrived == expected_per_occasion_) {
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
                     "[inline_prach_pipeline] WARN: slot evicted: id=%u age=%u "
                     "arrived=%u/%u\n",
                     b.slot_id,
                     age,
                     b.arrived,
                     expected_per_occasion_);
        prach_detection_result empty;
        empty.preambles.clear();
        if (cfg_.on_result_cb) {
          cfg_.on_result_cb(empty, b.slot_id);
        }
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
                 "[inline_prach_pipeline] WARN: bucket exhaustion, evicting slot=%u "
                 "(arrived=%u/%u) for incoming slot=%u\n",
                 victim.slot_id,
                 victim.arrived,
                 expected_per_occasion_,
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
    b.in_use  = false;
    b.slot_id = 0;
    b.arrived = 0;
    std::fill(b.desc_filled.begin(), b.desc_filled.end(), 0);
    b.mbuf_release_fns.clear();
  }

  void fire_occasion(slot_bucket& b)
  {
    cudaStream_t s = static_cast<cudaStream_t>(stream_);

    int rc = launch_bfp_decompress_re_demap(b.descs.data(),
                                            static_cast<prach_packet_descriptor*>(d_descs_),
                                            static_cast<unsigned>(b.descs.size()),
                                            cfg_.prb_bytes,
                                            cfg_.nof_prbs_per_packet,
                                            cfg_.data_width,
                                            cfg_.k_bar,
                                            cfg_.L_ra,
                                            cfg_.quantizer_gain,
                                            s);
    if (rc != 0) {
      std::fprintf(stderr, "[inline_prach_pipeline] BFP launch rc=%d slot=%u\n", rc, b.slot_id);
    }

    prach_detection_result result = detector_->detect_inline(*vram_buffer_, detect_cfg_);

    for (auto& fn : b.mbuf_release_fns) {
      if (fn) {
        fn();
      }
    }

    if (cfg_.on_result_cb) {
      cfg_.on_result_cb(result, b.slot_id);
    }

    reset_bucket(b);
  }

  config                                 cfg_;
  unsigned                               expected_per_occasion_ = 0;
  std::vector<slot_bucket>               buckets_;
  std::unique_ptr<vram_prach_buffer>     vram_buffer_;
  std::unique_ptr<prach_detector_inline> detector_;
  prach_detector::configuration          detect_cfg_{};
  void*                                  stream_  = nullptr;
  void*                                  d_descs_ = nullptr;
};

}

std::unique_ptr<inline_prach_pipeline> inline_prach_pipeline::create(const config& cfg)
{
  std::unique_ptr<inline_prach_pipeline_impl> p{new inline_prach_pipeline_impl(cfg)};
  if (!p->init()) {
    return nullptr;
  }
  return p;
}

}
}
}
