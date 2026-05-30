#include "ocudu/hal/cuda/srs_estimator_inline.h"
#include "ocudu/hal/cuda/vram_srs_buffer.h"

#include "ocudu/adt/complex.h"
#include "ocudu/ocudulog/ocudulog.h"
#include "ocudu/phy/generic_functions/generic_functions_factories.h"
#include "ocudu/phy/support/support_factories.h"
#include "ocudu/phy/support/time_alignment_estimator/time_alignment_estimator_factories.h"
#include "ocudu/phy/upper/sequence_generators/sequence_generator_factories.h"
#include "ocudu/phy/upper/signal_processors/srs/srs_estimator.h"
#include "ocudu/phy/upper/signal_processors/srs/srs_estimator_factory.h"
#include "ocudu/ran/srs/srs_information.h"
#include "fmt/core.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <complex>
#include <cstdio>
#include <cstdlib>
#include <cuda_runtime.h>
#include <vector>

using namespace ocudu;
using namespace ocudu::hal::cuda;

namespace {

constexpr unsigned NOF_SC_PER_PRB = 12;

srs_resource_configuration make_resource(unsigned nof_tx, unsigned nof_sym, unsigned comb,
                                         unsigned configuration_index, unsigned bandwidth_index)
{
  srs_resource_configuration r{};
  r.nof_antenna_ports   = static_cast<srs_resource_configuration::one_two_four_enum>(nof_tx);
  r.nof_symbols         = static_cast<srs_nof_symbols>(nof_sym);
  r.start_symbol        = 13;
  r.configuration_index = configuration_index;
  r.sequence_id         = 1;
  r.bandwidth_index     = bandwidth_index;
  r.comb_size           = (comb == 2) ? tx_comb_size::n2 : tx_comb_size::n4;
  r.comb_offset         = 0;
  r.cyclic_shift        = 0;
  r.freq_position       = 0;
  r.freq_shift          = 0;
  r.freq_hopping        = 0;
  r.hopping             = srs_group_or_sequence_hopping::neither;
  return r;
}

double percentile(std::vector<double>& v, double p)
{
  if (v.empty()) return 0.0;
  std::sort(v.begin(), v.end());
  size_t idx = static_cast<size_t>(p * (v.size() - 1));
  return v[idx];
}

struct bench_result {
  double cpu_mean, cpu_p50, cpu_p99;
  double gpu_mean, gpu_p50, gpu_p99;
  unsigned seq_len;
  bool   numerics_ok;
};

[[maybe_unused]] bench_result run_config(unsigned nof_rx, unsigned configuration_index, unsigned nof_reps,
                        ocudulog::basic_logger& logger)
{
  bench_result br{};
  const unsigned nof_tx  = 1;
  const unsigned nof_sym = 1;
  const unsigned comb    = 4;

  srs_resource_configuration resource = make_resource(nof_tx, nof_sym, comb, configuration_index, 0);
  auto           info    = get_srs_information(resource, 0);
  const unsigned seq_len = info.sequence_length;
  br.seq_len = seq_len;

  const unsigned nof_prbs = (seq_len * comb + NOF_SC_PER_PRB - 1) / NOF_SC_PER_PRB;
  const unsigned grid_subc = std::max(nof_prbs * NOF_SC_PER_PRB,
                                      info.mapping_initial_subcarrier + seq_len * comb);

  auto lpg_factory = create_low_papr_sequence_generator_sw_factory();
  auto lpg         = lpg_factory->create();
  std::vector<cf_t> ref_seq(seq_len);
  lpg->generate(ref_seq, info.sequence_group, info.sequence_number, info.n_cs, info.n_cs_max);

  std::vector<std::vector<cf_t>> rx_seq(nof_rx, std::vector<cf_t>(seq_len));
  srand(7);
  for (unsigned r = 0; r < nof_rx; ++r) {
    cf_t coeff = std::polar(0.5f + 0.1f * (r % 8), 0.2f * r - 0.5f);
    for (unsigned i = 0; i < seq_len; ++i) {
      cf_t sig   = ref_seq[i] * coeff * std::polar(1.0f, -0.03f * static_cast<float>(i));
      cf_t noise = cf_t((float(rand()) / RAND_MAX - 0.5f) * 0.005f,
                        (float(rand()) / RAND_MAX - 0.5f) * 0.005f);
      rx_seq[r][i] = to_cf(to_cbf16(sig + noise));
    }
  }

  auto grid_factory = create_resource_grid_factory();
  auto grid         = grid_factory->create(nof_rx, 14, grid_subc);
  for (unsigned r = 0; r < nof_rx; ++r) {
    for (unsigned i = 0; i < seq_len; ++i) {
      unsigned k = info.mapping_initial_subcarrier + comb * i;
      grid->get_writer().put(r, resource.start_symbol.value(), k, span<const cf_t>(&rx_seq[r][i], 1));
    }
  }

  vram_srs_buffer::config vcfg{nof_rx, nof_sym, seq_len};
  auto                    vbuf = vram_srs_buffer::create(vcfg);
  std::vector<cf_t>       flat(nof_rx * seq_len);
  for (unsigned r = 0; r < nof_rx; ++r)
    for (unsigned i = 0; i < seq_len; ++i)
      flat[r * seq_len + i] = rx_seq[r][i];
  cudaMemcpy(vbuf->device_base(), flat.data(), flat.size() * sizeof(cf_t), cudaMemcpyHostToDevice);

  auto dft_factory = create_dft_processor_factory_generic();
  auto ta_factory  = create_time_alignment_estimator_dft_factory(dft_factory);
  auto srs_factory = create_srs_estimator_generic_factory(lpg_factory, ta_factory, nof_prbs + 4);
  auto cpu_est     = srs_factory->create();

  srs_estimator_inline::config gpu_cfg{nof_rx, nof_tx, nof_sym, seq_len};
  auto                         gpu_est = srs_estimator_inline::create(gpu_cfg);
  if (!gpu_est) {
    fmt::print(stderr, "gpu estimator create failed for seq_len={}\n", seq_len);
    return br;
  }

  srs_estimator_configuration sr_cfg{};
  sr_cfg.slot     = slot_point(1, 0);
  sr_cfg.resource = resource;
  sr_cfg.ports.resize(nof_rx);
  for (unsigned r = 0; r < nof_rx; ++r) sr_cfg.ports[r] = static_cast<uint8_t>(r);

  for (unsigned w = 0; w < 32; ++w) {
    (void)cpu_est->estimate(grid->get_reader(), sr_cfg);
    (void)gpu_est->estimate_inline(*vbuf, sr_cfg);
  }

  auto cpu_chk = cpu_est->estimate(grid->get_reader(), sr_cfg);
  auto gpu_chk = gpu_est->estimate_inline(*vbuf, sr_cfg);
  float cn = cpu_chk.channel_matrix.frobenius_norm();
  float gn = gpu_chk.channel_matrix.frobenius_norm();
  br.numerics_ok = (cn > 0) && (std::fabs(cn - gn) / cn < 0.05f);

  std::vector<double> cpu_us, gpu_us;
  cpu_us.reserve(nof_reps);
  gpu_us.reserve(nof_reps);
  for (unsigned i = 0; i < nof_reps; ++i) {
    auto t0 = std::chrono::steady_clock::now();
    (void)cpu_est->estimate(grid->get_reader(), sr_cfg);
    auto t1 = std::chrono::steady_clock::now();
    cpu_us.push_back(std::chrono::duration<double, std::micro>(t1 - t0).count());
  }
  for (unsigned i = 0; i < nof_reps; ++i) {
    auto t0 = std::chrono::steady_clock::now();
    (void)gpu_est->estimate_inline(*vbuf, sr_cfg);
    auto t1 = std::chrono::steady_clock::now();
    gpu_us.push_back(std::chrono::duration<double, std::micro>(t1 - t0).count());
  }

  double cpu_sum = 0, gpu_sum = 0;
  for (double x : cpu_us) cpu_sum += x;
  for (double x : gpu_us) gpu_sum += x;
  br.cpu_mean = cpu_sum / cpu_us.size();
  br.gpu_mean = gpu_sum / gpu_us.size();
  br.cpu_p50  = percentile(cpu_us, 0.50);
  br.cpu_p99  = percentile(cpu_us, 0.99);
  br.gpu_p50  = percentile(gpu_us, 0.50);
  br.gpu_p99  = percentile(gpu_us, 0.99);
  return br;
}

void run_throughput(unsigned nof_rx, unsigned configuration_index, const std::vector<unsigned>& user_sweep,
                    unsigned nof_reps)
{
  const unsigned nof_tx  = 1;
  const unsigned nof_sym = 1;
  const unsigned comb    = 4;

  srs_resource_configuration resource = make_resource(nof_tx, nof_sym, comb, configuration_index, 0);
  auto           info    = get_srs_information(resource, 0);
  const unsigned seq_len = info.sequence_length;
  const unsigned nof_prbs = (seq_len * comb + NOF_SC_PER_PRB - 1) / NOF_SC_PER_PRB;
  const unsigned grid_subc = std::max(nof_prbs * NOF_SC_PER_PRB,
                                      info.mapping_initial_subcarrier + seq_len * comb);

  auto lpg_factory = create_low_papr_sequence_generator_sw_factory();
  auto lpg         = lpg_factory->create();
  std::vector<cf_t> ref_seq(seq_len);
  lpg->generate(ref_seq, info.sequence_group, info.sequence_number, info.n_cs, info.n_cs_max);

  std::vector<std::vector<cf_t>> rx_seq(nof_rx, std::vector<cf_t>(seq_len));
  srand(7);
  for (unsigned r = 0; r < nof_rx; ++r) {
    cf_t coeff = std::polar(0.5f + 0.1f * (r % 8), 0.2f * r - 0.5f);
    for (unsigned i = 0; i < seq_len; ++i) {
      cf_t sig   = ref_seq[i] * coeff * std::polar(1.0f, -0.03f * static_cast<float>(i));
      cf_t noise = cf_t((float(rand()) / RAND_MAX - 0.5f) * 0.005f,
                        (float(rand()) / RAND_MAX - 0.5f) * 0.005f);
      rx_seq[r][i] = to_cf(to_cbf16(sig + noise));
    }
  }

  auto grid_factory = create_resource_grid_factory();
  auto grid         = grid_factory->create(nof_rx, 14, grid_subc);
  for (unsigned r = 0; r < nof_rx; ++r)
    for (unsigned i = 0; i < seq_len; ++i) {
      unsigned k = info.mapping_initial_subcarrier + comb * i;
      grid->get_writer().put(r, resource.start_symbol.value(), k, span<const cf_t>(&rx_seq[r][i], 1));
    }

  vram_srs_buffer::config vcfg{nof_rx, nof_sym, seq_len};
  auto                    vbuf = vram_srs_buffer::create(vcfg);
  std::vector<cf_t>       flat(nof_rx * seq_len);
  for (unsigned r = 0; r < nof_rx; ++r)
    for (unsigned i = 0; i < seq_len; ++i)
      flat[r * seq_len + i] = rx_seq[r][i];
  cudaMemcpy(vbuf->device_base(), flat.data(), flat.size() * sizeof(cf_t), cudaMemcpyHostToDevice);

  auto dft_factory = create_dft_processor_factory_generic();
  auto ta_factory  = create_time_alignment_estimator_dft_factory(dft_factory);
  auto srs_factory = create_srs_estimator_generic_factory(lpg_factory, ta_factory, nof_prbs + 4);
  auto cpu_est     = srs_factory->create();

  const unsigned max_users = user_sweep.empty() ? 1u : *std::max_element(user_sweep.begin(), user_sweep.end());
  srs_estimator_inline::config gpu_cfg{nof_rx, nof_tx, nof_sym, seq_len, max_users};
  auto                         gpu_est = srs_estimator_inline::create(gpu_cfg);
  if (!gpu_est) {
    fmt::print(stderr, "gpu batch estimator create failed (max_users={})\n", max_users);
    return;
  }

  srs_estimator_configuration sr_cfg{};
  sr_cfg.slot     = slot_point(1, 0);
  sr_cfg.resource = resource;
  sr_cfg.ports.resize(nof_rx);
  for (unsigned r = 0; r < nof_rx; ++r) sr_cfg.ports[r] = static_cast<uint8_t>(r);

  std::vector<const vram_srs_buffer*>      bufs(max_users, vbuf.get());
  std::vector<srs_estimator_configuration> cfgs(max_users, sr_cfg);
  std::vector<srs_estimator_result>        res(max_users);

  gpu_est->estimate_inline_batch(bufs.data(), cfgs.data(), res.data(), max_users);
  auto  cpu_ref  = cpu_est->estimate(grid->get_reader(), sr_cfg);
  float ref_norm = cpu_ref.channel_matrix.frobenius_norm();
  for (unsigned o : {0u, max_users - 1}) {
    float gn = res[o].channel_matrix.frobenius_norm();
    if (ref_norm <= 0 || std::fabs(gn - ref_norm) / ref_norm > 0.05f) {
      fmt::print(stderr, "[BATCH MISMATCH] occ={} gpu_norm={:.4f} cpu_norm={:.4f}\n", o, gn, ref_norm);
    }
  }

  fmt::print("\n-- Throughput sweep · {} RX · seq_len {} ({} PRB) --\n",
             nof_rx, seq_len, nof_prbs);
  fmt::print("{:>6} | {:>10} {:>10} | {:>11} {:>11} | {:>9}\n",
             "#UEs", "cpu/occ", "gpu/occ", "cpu_kocc/s", "gpu_kocc/s", "speedup");
  fmt::print("{:->6}-+-{:->10} {:->10}-+-{:->11} {:->11}-+-{:->9}\n", "", "", "", "", "", "");

  for (unsigned N : user_sweep) {

    const bool have_graph = gpu_est->build_batch_graph(bufs.data(), cfgs.data(), N);

    for (unsigned w = 0; w < 16; ++w) {
      gpu_est->estimate_inline_batch(bufs.data(), cfgs.data(), res.data(), N);
      if (have_graph) gpu_est->run_batch_graph(res.data());
      for (unsigned u = 0; u < N; ++u) (void)cpu_est->estimate(grid->get_reader(), sr_cfg);
    }

    double cpu_total = 0, loop_total = 0, graph_total = 0;
    for (unsigned rep = 0; rep < nof_reps; ++rep) {
      auto t0 = std::chrono::steady_clock::now();
      for (unsigned u = 0; u < N; ++u) (void)cpu_est->estimate(grid->get_reader(), sr_cfg);
      auto t1 = std::chrono::steady_clock::now();
      cpu_total += std::chrono::duration<double, std::micro>(t1 - t0).count();
    }
    for (unsigned rep = 0; rep < nof_reps; ++rep) {
      auto t0 = std::chrono::steady_clock::now();
      gpu_est->estimate_inline_batch(bufs.data(), cfgs.data(), res.data(), N);
      auto t1 = std::chrono::steady_clock::now();
      loop_total += std::chrono::duration<double, std::micro>(t1 - t0).count();
    }
    for (unsigned rep = 0; rep < nof_reps && have_graph; ++rep) {
      auto t0 = std::chrono::steady_clock::now();
      gpu_est->run_batch_graph(res.data());
      auto t1 = std::chrono::steady_clock::now();
      graph_total += std::chrono::duration<double, std::micro>(t1 - t0).count();
    }
    const double cpu_per_occ   = cpu_total / (nof_reps * N);
    const double loop_per_occ  = loop_total / (nof_reps * N);
    const double graph_per_occ = have_graph ? graph_total / (nof_reps * N) : 0.0;
    fmt::print("{:>6} | {:>9.2f}u {:>9.2f}u | {:>11.1f} {:>11.1f} | {:>8.2f}x\n",
               N, cpu_per_occ, graph_per_occ,
               1e3 / cpu_per_occ,
               graph_per_occ > 0 ? 1e3 / graph_per_occ : 0.0,
               graph_per_occ > 0 ? cpu_per_occ / graph_per_occ : 0.0);
    (void)loop_per_occ;
  }
}

}

int main(int argc, char** argv)
{
  ocudulog::init();
  auto& logger = ocudulog::fetch_basic_logger("SRS_INLINE_BENCH", true);
  logger.set_level(ocudulog::basic_levels::warning);

  const unsigned nof_reps = (argc > 1) ? std::strtoul(argv[1], nullptr, 10) : 2000;

  fmt::print("================================================================================\n");
  fmt::print("  Inline GPU SRS — benchmark  (4 RX, captured CUDA graph, reps/config={})\n", nof_reps);
  fmt::print("================================================================================\n");

  const std::vector<unsigned> user_sweep = {1, 2, 4, 8, 16, 32, 64, 128, 256};
  const unsigned tput_reps = std::max(50u, nof_reps / 8);
  run_throughput(4, 0,  user_sweep, tput_reps);
  run_throughput(4, 17, user_sweep, tput_reps);
  run_throughput(4, 45, user_sweep, tput_reps);

  fmt::print("\n-- CPU time freed per slot (slot budget = 500 us, 30 kHz TDD) --\n");
  fmt::print("{:>6} | {:>12} {:>12} | {:>11}\n",
             "#UEs", "cpu_serial", "gpu_dispatch", "offload");
  fmt::print("{:->6}-+-{:->12} {:->12}-+-{:->11}\n", "", "", "", "");

  {
    const unsigned nof_rx = 4, nof_tx = 1, nof_sym = 1, comb = 4;
    const unsigned ci = 17;

    srs_resource_configuration resource = make_resource(nof_tx, nof_sym, comb, ci, 0);
    auto           info    = get_srs_information(resource, 0);
    const unsigned seq_len = info.sequence_length;
    const unsigned nof_prbs = (seq_len * comb + NOF_SC_PER_PRB - 1) / NOF_SC_PER_PRB;
    const unsigned grid_subc = std::max(nof_prbs * NOF_SC_PER_PRB,
                                        info.mapping_initial_subcarrier + seq_len * comb);

    auto lpg_factory = create_low_papr_sequence_generator_sw_factory();
    auto lpg         = lpg_factory->create();
    std::vector<cf_t> ref_seq(seq_len);
    lpg->generate(ref_seq, info.sequence_group, info.sequence_number, info.n_cs, info.n_cs_max);

    std::vector<std::vector<cf_t>> rx_seq(nof_rx, std::vector<cf_t>(seq_len));
    srand(7);
    for (unsigned r = 0; r < nof_rx; ++r) {
      cf_t coeff = std::polar(0.5f + 0.1f * (r % 8), 0.2f * r - 0.5f);
      for (unsigned i = 0; i < seq_len; ++i) {
        cf_t sig   = ref_seq[i] * coeff * std::polar(1.0f, -0.03f * static_cast<float>(i));
        cf_t noise = cf_t((float(rand()) / RAND_MAX - 0.5f) * 0.005f,
                          (float(rand()) / RAND_MAX - 0.5f) * 0.005f);
        rx_seq[r][i] = to_cf(to_cbf16(sig + noise));
      }
    }

    auto grid_factory = create_resource_grid_factory();
    auto grid         = grid_factory->create(nof_rx, 14, grid_subc);
    for (unsigned r = 0; r < nof_rx; ++r)
      for (unsigned i = 0; i < seq_len; ++i) {
        unsigned k = info.mapping_initial_subcarrier + comb * i;
        grid->get_writer().put(r, resource.start_symbol.value(), k, span<const cf_t>(&rx_seq[r][i], 1));
      }

    vram_srs_buffer::config vcfg{nof_rx, nof_sym, seq_len};
    auto                    vbuf = vram_srs_buffer::create(vcfg);
    std::vector<cf_t>       flat(nof_rx * seq_len);
    for (unsigned r = 0; r < nof_rx; ++r)
      for (unsigned i = 0; i < seq_len; ++i)
        flat[r * seq_len + i] = rx_seq[r][i];
    cudaMemcpy(vbuf->device_base(), flat.data(), flat.size() * sizeof(cf_t), cudaMemcpyHostToDevice);

    auto dft_factory = create_dft_processor_factory_generic();
    auto ta_factory  = create_time_alignment_estimator_dft_factory(dft_factory);
    auto srs_factory = create_srs_estimator_generic_factory(lpg_factory, ta_factory, nof_prbs + 4);
    auto cpu_est     = srs_factory->create();

    srs_estimator_configuration sr_cfg{};
    sr_cfg.slot     = slot_point(1, 0);
    sr_cfg.resource = resource;
    sr_cfg.ports.resize(nof_rx);
    for (unsigned r = 0; r < nof_rx; ++r) sr_cfg.ports[r] = static_cast<uint8_t>(r);

    const std::vector<unsigned> offload_sweep = {1, 4, 8, 16, 32, 64};
    const unsigned               oreps        = std::max(50u, nof_reps / 4);
    const double                 slot_budget  = 500.0;

    for (unsigned N : offload_sweep) {
      srs_estimator_inline::config gpu_cfg{nof_rx, nof_tx, nof_sym, seq_len, N};
      auto                         gpu_est = srs_estimator_inline::create(gpu_cfg);
      if (!gpu_est) { continue; }

      std::vector<const vram_srs_buffer*>      bufs(N, vbuf.get());
      std::vector<srs_estimator_configuration> cfgs(N, sr_cfg);
      std::vector<srs_estimator_result>        res(N);
      if (!gpu_est->build_batch_graph(bufs.data(), cfgs.data(), N)) { continue; }

      for (unsigned w = 0; w < 16; ++w) {
        gpu_est->run_batch_graph(res.data());
        for (unsigned u = 0; u < N; ++u) (void)cpu_est->estimate(grid->get_reader(), sr_cfg);
      }

      double gpu_wall_total = 0;
      for (unsigned rep = 0; rep < oreps; ++rep) {
        auto t0 = std::chrono::steady_clock::now();
        gpu_est->run_batch_graph(res.data());
        auto t1 = std::chrono::steady_clock::now();
        gpu_wall_total += std::chrono::duration<double, std::micro>(t1 - t0).count();
      }
      const double gpu_wall_us = gpu_wall_total / oreps;

      double cpu_total = 0;
      for (unsigned rep = 0; rep < oreps; ++rep) {
        auto t0 = std::chrono::steady_clock::now();
        for (unsigned u = 0; u < N; ++u) (void)cpu_est->estimate(grid->get_reader(), sr_cfg);
        auto t1 = std::chrono::steady_clock::now();
        cpu_total += std::chrono::duration<double, std::micro>(t1 - t0).count();
      }
      const double cpu_total_us = cpu_total / oreps;

      double dispatch_total = 0;
      for (unsigned rep = 0; rep < oreps; ++rep) {
        auto t0 = std::chrono::steady_clock::now();
        gpu_est->run_batch_graph_async();
        auto t1 = std::chrono::steady_clock::now();
        dispatch_total += std::chrono::duration<double, std::micro>(t1 - t0).count();

        cudaDeviceSynchronize();
      }
      const double dispatch_us   = dispatch_total / oreps;
      const double offload_ratio = cpu_total_us / dispatch_us;
      (void)gpu_wall_us;
      (void)slot_budget;

      fmt::print("{:>6} | {:>11.1f}u {:>11.1f}u | {:>10.1f}x\n",
                 N, cpu_total_us, dispatch_us, offload_ratio);
    }
  }
  return 0;
}
