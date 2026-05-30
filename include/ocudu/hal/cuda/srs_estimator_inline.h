#pragma once

#include "ocudu/hal/cuda/vram_srs_buffer.h"
#include "ocudu/phy/upper/signal_processors/srs/srs_estimator_configuration.h"
#include "ocudu/phy/upper/signal_processors/srs/srs_estimator_result.h"
#include <memory>

namespace ocudu {
namespace hal {
namespace cuda {

class srs_estimator_inline
{
public:

  struct config {
    unsigned max_nof_rx_ports;
    unsigned max_nof_antenna_ports;
    unsigned max_nof_symbols;
    unsigned max_sequence_length;

    unsigned max_batch = 1;
  };

  static std::unique_ptr<srs_estimator_inline> create(const config& cfg);

  virtual ~srs_estimator_inline() = default;

  virtual srs_estimator_result estimate_inline(const vram_srs_buffer&             vram_buf,
                                               const srs_estimator_configuration& cfg) = 0;

  virtual unsigned estimate_inline_batch(const vram_srs_buffer* const*      vram_bufs,
                                         const srs_estimator_configuration* cfgs,
                                         srs_estimator_result*              results,
                                         unsigned                           nof_occasions) = 0;

  virtual bool build_batch_graph(const vram_srs_buffer* const*      vram_bufs,
                                 const srs_estimator_configuration* cfgs,
                                 unsigned                           nof_occasions) = 0;

  virtual unsigned run_batch_graph(srs_estimator_result* results) = 0;

  virtual unsigned run_batch_graph_async() = 0;

  srs_estimator_inline(const srs_estimator_inline&)            = delete;
  srs_estimator_inline& operator=(const srs_estimator_inline&) = delete;

protected:
  srs_estimator_inline() = default;
};

}
}
}
