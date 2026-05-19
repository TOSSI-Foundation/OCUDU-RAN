#include "ocudu/support/mac_phy_handoff_timing.h"

#include <atomic>
#include <cstdio>
#include <ctime>
#include <mutex>
#include <string>
#include <sys/stat.h>
#include <sys/types.h>

namespace ocudu::mac_phy_handoff_timing {

namespace {

struct entry {
  uint64_t  ts_ns;
  uint16_t  sfn;
  uint16_t  slot;
  uint8_t   kind;
  uint8_t   dir;
  uint16_t  _pad;
};
static_assert(sizeof(entry) == 16, "entry layout drift will change CSV row count math");

std::atomic<bool>     g_enabled{false};
entry*                g_buffer  = nullptr;
std::size_t           g_capacity = 0;
std::atomic<uint64_t> g_index{0};
std::atomic<uint64_t> g_dropped{0};
std::string           g_output_path;
std::mutex            g_lifecycle_mu; // guards initialize / dump / shutdown only.

const char* kind_to_str(uint8_t k)
{
  switch (static_cast<msg_kind>(k)) {
    case msg_kind::DL_TTI_REQUEST:  return "DL_TTI_REQUEST";
    case msg_kind::UL_TTI_REQUEST:  return "UL_TTI_REQUEST";
    case msg_kind::UL_DCI_REQUEST:  return "UL_DCI_REQUEST";
    case msg_kind::TX_DATA_REQUEST: return "TX_DATA_REQUEST";
  }
  return "UNKNOWN";
}

const char* dir_to_str(uint8_t d)
{
  switch (static_cast<direction>(d)) {
    case direction::TX_L2: return "TX_L2";
    case direction::RX_L1: return "RX_L1";
  }
  return "UNKNOWN";
}

uint64_t now_monotonic_ns()
{
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return static_cast<uint64_t>(ts.tv_sec) * 1000000000ULL + static_cast<uint64_t>(ts.tv_nsec);
}

void ensure_parent_dir(const std::string& path)
{
  std::size_t slash = path.find_last_of('/');
  if (slash == std::string::npos || slash == 0) {
    return;
  }
  std::string parent = path.substr(0, slash);
  std::size_t pos = 1;
  while (pos <= parent.size()) {
    std::size_t next    = parent.find('/', pos);
    std::string segment = parent.substr(0, (next == std::string::npos) ? parent.size() : next);
    ::mkdir(segment.c_str(), 0755);
    if (next == std::string::npos) {
      break;
    }
    pos = next + 1;
  }
}

} // namespace

void initialize(bool enabled, const std::string& output_path, std::size_t capacity)
{
  std::lock_guard<std::mutex> lock(g_lifecycle_mu);

  if (g_enabled.load(std::memory_order_relaxed)) {
    return; // already initialised
  }
  if (!enabled || capacity == 0) {
    return;
  }

  g_buffer = new (std::nothrow) entry[capacity];
  if (g_buffer == nullptr) {
    std::fprintf(stderr,
                 "[handoff-timing] failed to allocate %zu entries (%zu bytes); recorder disabled\n",
                 capacity, capacity * sizeof(entry));
    return;
  }

  g_capacity    = capacity;
  g_output_path = output_path;
  g_index.store(0, std::memory_order_relaxed);
  g_dropped.store(0, std::memory_order_relaxed);
  g_enabled.store(true, std::memory_order_release);
}

bool is_enabled()
{
  return g_enabled.load(std::memory_order_acquire);
}

void record(msg_kind kind, direction dir, uint16_t sfn, uint16_t slot)
{
  if (!g_enabled.load(std::memory_order_acquire)) {
    return;
  }
  const uint64_t idx = g_index.fetch_add(1, std::memory_order_relaxed);
  if (idx >= g_capacity) {
    g_dropped.fetch_add(1, std::memory_order_relaxed);
    return;
  }
  entry& e = g_buffer[idx];
  e.ts_ns = now_monotonic_ns();
  e.sfn   = sfn;
  e.slot  = slot;
  e.kind  = static_cast<uint8_t>(kind);
  e.dir   = static_cast<uint8_t>(dir);
  e._pad  = 0;
}

void dump_to_csv()
{
  std::lock_guard<std::mutex> lock(g_lifecycle_mu);

  if (!g_enabled.load(std::memory_order_acquire) || g_buffer == nullptr) {
    return;
  }

  const uint64_t reserved = g_index.load(std::memory_order_acquire);
  const uint64_t written  = (reserved < g_capacity) ? reserved : g_capacity;
  const uint64_t dropped  = g_dropped.load(std::memory_order_relaxed) +
                            (reserved > g_capacity ? (reserved - g_capacity) : 0);

  ensure_parent_dir(g_output_path);
  FILE* fp = std::fopen(g_output_path.c_str(), "w");
  if (fp == nullptr) {
    std::fprintf(stderr,
                 "[handoff-timing] could not open '%s' for write\n",
                 g_output_path.c_str());
    return;
  }

  std::fprintf(fp, "timestamp_ns,direction,msg_type,sfn,slot\n");
  for (uint64_t i = 0; i < written; ++i) {
    const entry& e = g_buffer[i];
    std::fprintf(fp,
                 "%llu,%s,%s,%u,%u\n",
                 static_cast<unsigned long long>(e.ts_ns),
                 dir_to_str(e.dir),
                 kind_to_str(e.kind),
                 static_cast<unsigned>(e.sfn),
                 static_cast<unsigned>(e.slot));
  }
  std::fclose(fp);

  std::fprintf(stderr,
               "[handoff-timing] wrote %llu entries to %s (dropped=%llu)\n",
               static_cast<unsigned long long>(written),
               g_output_path.c_str(),
               static_cast<unsigned long long>(dropped));
}

void shutdown()
{
  std::lock_guard<std::mutex> lock(g_lifecycle_mu);

  g_enabled.store(false, std::memory_order_release);
  delete[] g_buffer;
  g_buffer   = nullptr;
  g_capacity = 0;
  g_index.store(0, std::memory_order_relaxed);
  g_dropped.store(0, std::memory_order_relaxed);
  g_output_path.clear();
}

} // namespace ocudu::mac_phy_handoff_timing
