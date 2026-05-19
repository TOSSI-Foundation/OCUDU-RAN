#include "ocudu/support/mac_phy_latency_injector.h"

#include <atomic>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <random>

namespace ocudu::mac_phy_latency_injector {

namespace {

constexpr uint64_t MAX_DELAY_NS         = 50ULL * 1000ULL * 1000ULL; // 50 ms
constexpr uint64_t HYBRID_THRESHOLD_NS  = 100ULL * 1000ULL;          // 100 us
constexpr uint64_t SPIN_TAIL_NS         = 50ULL * 1000ULL;           // 50 us
constexpr uint32_t MAX_JITTER_PCT       = 99;                        // keep min > 0

std::atomic<bool>     g_enabled{false};
std::atomic<uint64_t> g_mean_dl_tti{0};
std::atomic<uint64_t> g_mean_ul_tti{0};
std::atomic<uint64_t> g_mean_ul_dci{0};
std::atomic<uint64_t> g_mean_tx_data{0};
std::atomic<uint32_t> g_jitter_pct{0};

thread_local std::mt19937_64 t_rng{std::random_device{}()};

uint64_t now_monotonic_ns()
{
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return static_cast<uint64_t>(ts.tv_sec) * 1000000000ULL + static_cast<uint64_t>(ts.tv_nsec);
}

uint64_t parse_env_ns(const char* name, uint64_t fallback)
{
  const char* v = std::getenv(name);
  if (v == nullptr || *v == '\0') {
    return fallback;
  }
  char*    end = nullptr;
  uint64_t val = std::strtoull(v, &end, 10);
  if (end == v) {
    std::fprintf(stderr,
                 "[latency-injector] WARNING: %s='%s' is not a number; using default %llu ns\n",
                 name, v, static_cast<unsigned long long>(fallback));
    return fallback;
  }
  if (val > MAX_DELAY_NS) {
    std::fprintf(stderr,
                 "[latency-injector] WARNING: %s=%llu ns exceeds cap %llu ns; clamping\n",
                 name,
                 static_cast<unsigned long long>(val),
                 static_cast<unsigned long long>(MAX_DELAY_NS));
    val = MAX_DELAY_NS;
  }
  return val;
}

bool parse_env_bool(const char* name, bool fallback)
{
  const char* v = std::getenv(name);
  if (v == nullptr || *v == '\0') {
    return fallback;
  }
  return std::strcmp(v, "0") != 0 && std::strcmp(v, "false") != 0 && std::strcmp(v, "FALSE") != 0;
}

void busy_wait_until(uint64_t deadline_ns)
{
  while (now_monotonic_ns() < deadline_ns) {
  }
}

void hybrid_wait(uint64_t delay_ns)
{
  if (delay_ns == 0) {
    return;
  }

  const uint64_t start    = now_monotonic_ns();
  const uint64_t deadline = start + delay_ns;

  if (delay_ns < HYBRID_THRESHOLD_NS) {
    busy_wait_until(deadline);
    return;
  }

  const uint64_t sleep_until_ns = deadline - SPIN_TAIL_NS;
  struct timespec ts;
  ts.tv_sec  = static_cast<time_t>(sleep_until_ns / 1000000000ULL);
  ts.tv_nsec = static_cast<long>(sleep_until_ns % 1000000000ULL);

  while (true) {
    int rc = clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &ts, nullptr);
    if (rc == 0 || rc != EINTR) {
      break;
    }
  }
  busy_wait_until(deadline);
}

} // namespace

void initialize(bool     enabled,
                uint64_t dl_tti_mean_ns,
                uint64_t ul_tti_mean_ns,
                uint64_t ul_dci_mean_ns,
                uint64_t tx_data_mean_ns,
                uint32_t jitter_pct)
{
  if (jitter_pct > MAX_JITTER_PCT) {
    std::fprintf(stderr,
                 "[latency-injector] WARNING: jitter_pct=%u exceeds %u; clamping\n",
                 jitter_pct, MAX_JITTER_PCT);
    jitter_pct = MAX_JITTER_PCT;
  }

  g_mean_dl_tti.store(dl_tti_mean_ns,  std::memory_order_relaxed);
  g_mean_ul_tti.store(ul_tti_mean_ns,  std::memory_order_relaxed);
  g_mean_ul_dci.store(ul_dci_mean_ns,  std::memory_order_relaxed);
  g_mean_tx_data.store(tx_data_mean_ns, std::memory_order_relaxed);
  g_jitter_pct.store(jitter_pct, std::memory_order_relaxed);

  const bool any_nonzero = (dl_tti_mean_ns | ul_tti_mean_ns | ul_dci_mean_ns | tx_data_mean_ns) != 0;
  const bool active      = enabled && any_nonzero;
  g_enabled.store(active, std::memory_order_release);

  if (active) {
    auto band = [jitter_pct](uint64_t mean) {
      const uint64_t span = (mean * jitter_pct) / 100;
      const uint64_t lo   = mean > span ? mean - span : 0;
      const uint64_t hi   = mean + span;
      return std::pair<uint64_t, uint64_t>{lo, hi};
    };
    auto dl  = band(dl_tti_mean_ns);
    auto ul  = band(ul_tti_mean_ns);
    auto ud  = band(ul_dci_mean_ns);
    auto td  = band(tx_data_mean_ns);
    std::fprintf(stderr,
                 "[latency-injector] ACTIVE (random uniform, jitter=+/-%u%%, "
                 "cap=%llu ns, hybrid_threshold=%llu ns):\n"
                 "  DL_TTI : mean=%llu ns range=[%llu, %llu] ns\n"
                 "  UL_TTI : mean=%llu ns range=[%llu, %llu] ns\n"
                 "  UL_DCI : mean=%llu ns range=[%llu, %llu] ns\n"
                 "  TX_DATA: mean=%llu ns range=[%llu, %llu] ns\n",
                 jitter_pct,
                 static_cast<unsigned long long>(MAX_DELAY_NS),
                 static_cast<unsigned long long>(HYBRID_THRESHOLD_NS),
                 static_cast<unsigned long long>(dl_tti_mean_ns),
                 static_cast<unsigned long long>(dl.first),
                 static_cast<unsigned long long>(dl.second),
                 static_cast<unsigned long long>(ul_tti_mean_ns),
                 static_cast<unsigned long long>(ul.first),
                 static_cast<unsigned long long>(ul.second),
                 static_cast<unsigned long long>(ul_dci_mean_ns),
                 static_cast<unsigned long long>(ud.first),
                 static_cast<unsigned long long>(ud.second),
                 static_cast<unsigned long long>(tx_data_mean_ns),
                 static_cast<unsigned long long>(td.first),
                 static_cast<unsigned long long>(td.second));
  } else {
    std::fprintf(stderr, "[latency-injector] disabled\n");
  }
}

void initialize_from_env()
{

  const uint64_t dl_tti_ns  = parse_env_ns("OCUDU_INJECT_DL_TTI_NS",  300000);
  const uint64_t ul_tti_ns  = parse_env_ns("OCUDU_INJECT_UL_TTI_NS",  300000);
  const uint64_t ul_dci_ns  = parse_env_ns("OCUDU_INJECT_UL_DCI_NS",  300000);
  const uint64_t tx_data_ns = parse_env_ns("OCUDU_INJECT_TX_DATA_NS", 600000);
  const uint64_t jitter_raw = parse_env_ns("OCUDU_INJECT_JITTER_PCT", 50);
  const uint32_t jitter_pct = static_cast<uint32_t>(
      jitter_raw > MAX_JITTER_PCT ? MAX_JITTER_PCT : jitter_raw);
  const bool     enabled    = parse_env_bool("OCUDU_INJECT_ENABLE", true);

  initialize(enabled, dl_tti_ns, ul_tti_ns, ul_dci_ns, tx_data_ns, jitter_pct);
}

bool is_enabled()
{
  return g_enabled.load(std::memory_order_acquire);
}

void inject(msg_kind kind)
{
  if (!g_enabled.load(std::memory_order_acquire)) {
    return;
  }
  uint64_t mean_ns = 0;
  switch (kind) {
    case msg_kind::DL_TTI_REQUEST:  mean_ns = g_mean_dl_tti.load(std::memory_order_relaxed);  break;
    case msg_kind::UL_TTI_REQUEST:  mean_ns = g_mean_ul_tti.load(std::memory_order_relaxed);  break;
    case msg_kind::UL_DCI_REQUEST:  mean_ns = g_mean_ul_dci.load(std::memory_order_relaxed);  break;
    case msg_kind::TX_DATA_REQUEST: mean_ns = g_mean_tx_data.load(std::memory_order_relaxed); break;
  }
  if (mean_ns == 0) {
    return;
  }

  const uint32_t jitter_pct = g_jitter_pct.load(std::memory_order_relaxed);
  uint64_t delay_ns = mean_ns;
  if (jitter_pct != 0) {
    const uint64_t span = (mean_ns * jitter_pct) / 100;
    const uint64_t lo   = mean_ns > span ? mean_ns - span : 0;
    const uint64_t hi   = mean_ns + span;
    std::uniform_int_distribution<uint64_t> dist(lo, hi);
    delay_ns = dist(t_rng);
  }
  hybrid_wait(delay_ns);
}

void shutdown()
{
  g_enabled.store(false, std::memory_order_release);
  g_mean_dl_tti.store(0, std::memory_order_relaxed);
  g_mean_ul_tti.store(0, std::memory_order_relaxed);
  g_mean_ul_dci.store(0, std::memory_order_relaxed);
  g_mean_tx_data.store(0, std::memory_order_relaxed);
  g_jitter_pct.store(0, std::memory_order_relaxed);
}

} // namespace ocudu::mac_phy_latency_injector
