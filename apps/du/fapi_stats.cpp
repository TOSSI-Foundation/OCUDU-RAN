#include "fapi_stats.h"

#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <string>
#include <system_error>

namespace {


ocudu::fapi_stats::message_stat* g_buf = nullptr;


std::atomic<uint64_t> g_index{0};

std::string g_output_path;
bool        g_enabled      = false;
bool        g_dump_done    = false;


std::string apply_timestamp_suffix(const std::string& path)
{
  char       stamp[32];
  std::time_t t = std::time(nullptr);
  struct tm  lt {};
  ::localtime_r(&t, &lt);
  std::strftime(stamp, sizeof(stamp), "_%Y%m%d_%H%M%S", &lt);

  const std::string ext = ".json";
  if (path.size() >= ext.size() && path.compare(path.size() - ext.size(), ext.size(), ext) == 0) {
    return path.substr(0, path.size() - ext.size()) + stamp + ext;
  }
  return path + stamp;
}


void mkdirs_for_file(const std::string& path)
{
  namespace fs = std::filesystem;
  fs::path      p(path);
  const fs::path parent = p.parent_path();
  if (parent.empty()) {
    return;
  }
  std::error_code ec;
  fs::create_directories(parent, ec);
}


void escape_json_string(std::FILE* fp, const char* str)
{
  for (const char* p = str; *p; ++p) {
    switch (*p) {
      case '\n': std::fputs("\\n", fp); break;
      case '\r': std::fputs("\\r", fp); break;
      case '\t': std::fputs("\\t", fp); break;
      case '\\': std::fputs("\\\\", fp); break;
      case '"':  std::fputs("\\\"", fp); break;
      case '\b': std::fputs("\\b", fp); break;
      case '\f': std::fputs("\\f", fp); break;
      default:
        if (static_cast<unsigned char>(*p) < 32) {
          std::fprintf(fp, "\\u%04x", static_cast<unsigned char>(*p));
        } else {
          std::fputc(*p, fp);
        }
        break;
    }
  }
}

} // namespace

namespace ocudu::fapi_stats {

uint64_t timestamp_ns()
{
  struct timespec ts {};
  ::clock_gettime(CLOCK_REALTIME, &ts);
  return static_cast<uint64_t>(ts.tv_sec) * 1000000000ULL + static_cast<uint64_t>(ts.tv_nsec);
}

void initialize(bool enabled, const std::string& output_path, bool add_timestamp)
{

  g_output_path = add_timestamp ? apply_timestamp_suffix(output_path) : output_path;
  g_enabled     = enabled;
  if (!enabled) {
    std::fprintf(stderr, "[FAPI-STATS] disabled (no recording, no RAM allocated). "
                         "Check your YAML has 'fapi_stats: { enabled: true }'.\n");
    return;
  }


  g_buf = new message_stat[MAX_MESSAGE_STATS]();
  g_index.store(0, std::memory_order_relaxed);

  const double mib = (static_cast<double>(sizeof(message_stat)) * MAX_MESSAGE_STATS) / (1024.0 * 1024.0);
  std::fprintf(stderr, "[FAPI-STATS] enabled: capacity=%zu entries, ~%.0f MiB, output=%s\n",
               MAX_MESSAGE_STATS, mib, g_output_path.c_str());


  static bool atexit_registered = false;
  if (!atexit_registered) {
    std::atexit([]() {
      ocudu::fapi_stats::dump_to_json();
    });
    atexit_registered = true;
  }
}

bool is_enabled()
{
  return g_enabled && g_buf != nullptr;
}

void record(const char* msg_type,
            const char* direction,
            int         sfn,
            int         slot,
            int         pdu_size,
            int         num_pdus,
            uint64_t    ipc_latency,
            const char* content)
{
  if (!is_enabled() || msg_type == nullptr) {
    return;
  }


  const uint64_t idx = g_index.fetch_add(1, std::memory_order_relaxed);
  if (idx >= MAX_MESSAGE_STATS) {
    return;
  }

  message_stat* s   = &g_buf[idx];
  s->timestamp_ns   = timestamp_ns();
  s->ipc_latency_ns = ipc_latency;
  std::strncpy(s->message_type, msg_type, MAX_MESSAGE_TYPE_LEN - 1);
  s->message_type[MAX_MESSAGE_TYPE_LEN - 1] = '\0';
  if (direction != nullptr) {
    std::strncpy(s->direction, direction, MAX_DIRECTION_LEN - 1);
    s->direction[MAX_DIRECTION_LEN - 1] = '\0';
  } else {
    s->direction[0] = '\0';
  }
  s->sfn      = sfn;
  s->slot     = slot;
  s->pdu_size = pdu_size;
  s->num_pdus = num_pdus;
  if (content != nullptr) {
    std::strncpy(s->message_content, content, MAX_MESSAGE_CONTENT_LEN - 1);
    s->message_content[MAX_MESSAGE_CONTENT_LEN - 1] = '\0';
  } else {
    s->message_content[0] = '\0';
  }
}

void dump_to_json()
{
  if (g_dump_done) {
    return;
  }
  if (!g_enabled || g_buf == nullptr) {

    std::fprintf(stderr, "[FAPI-STATS] dump_to_json: recorder was not enabled; no file written\n");
    g_dump_done = true;
    return;
  }

  std::fprintf(stderr, "[FAPI-STATS] Writing JSON dump to %s...\n", g_output_path.c_str());


  mkdirs_for_file(g_output_path);

  std::FILE* fp = std::fopen(g_output_path.c_str(), "w");
  if (fp == nullptr) {
    std::fprintf(stderr, "[FAPI-STATS] ERROR: could not open %s for writing (errno=%d). "
                         "Check the parent directory exists and the process has write access.\n",
                 g_output_path.c_str(), errno);
    g_dump_done = true;
    return;
  }

  const uint64_t total          = g_index.load(std::memory_order_relaxed);
  const uint64_t dumped         = (total > MAX_MESSAGE_STATS) ? MAX_MESSAGE_STATS : total;
  const uint64_t total_attempts = total; // includes any writes past capacity that were dropped

  std::fprintf(fp, "{\n");
  std::fprintf(fp, "  \"total_messages_captured\": %lu,\n", static_cast<unsigned long>(total_attempts));
  std::fprintf(fp, "  \"messages_in_dump\": %lu,\n", static_cast<unsigned long>(dumped));
  std::fprintf(fp, "  \"messages\": [\n");

  bool first = true;
  for (uint64_t i = 0; i < dumped; ++i) {
    message_stat* s = &g_buf[i];
    if (s->timestamp_ns == 0) {
      continue;
    }

    if (!first) {
      std::fputs(",\n", fp);
    }
    first = false;


    std::fprintf(fp, "    {\n");
    std::fprintf(fp, "      \"timestamp_ns\": %lu,\n", static_cast<unsigned long>(s->timestamp_ns));
    std::fprintf(fp, "      \"message_type\": \"%s\",\n", s->message_type);
    std::fprintf(fp, "      \"direction\": \"%s\",\n", s->direction);
    std::fprintf(fp, "      \"sfn\": %d,\n", s->sfn);
    std::fprintf(fp, "      \"slot\": %d,\n", s->slot);
    std::fprintf(fp, "      \"pdu_size\": %d,\n", s->pdu_size);
    std::fprintf(fp, "      \"num_pdus\": %d,\n", s->num_pdus);
    std::fprintf(fp, "      \"ipc_latency_ns\": %lu,\n", static_cast<unsigned long>(s->ipc_latency_ns));
    std::fprintf(fp, "      \"message_content\": \"");
    escape_json_string(fp, s->message_content);
    std::fprintf(fp, "\"\n");
    std::fprintf(fp, "    }");
  }

  std::fprintf(fp, "\n  ]\n}\n");
  std::fclose(fp);

  g_dump_done = true;
  std::fprintf(stderr, "[FAPI-STATS] Dumped %lu messages to %s (total attempts=%lu, dropped=%lu)\n",
               static_cast<unsigned long>(dumped),
               g_output_path.c_str(),
               static_cast<unsigned long>(total_attempts),
               static_cast<unsigned long>(total_attempts > dumped ? total_attempts - dumped : 0));
}

void shutdown()
{
  if (g_buf != nullptr) {
    delete[] g_buf;
    g_buf = nullptr;
  }
  g_enabled = false;
}

} // namespace ocudu::fapi_stats
