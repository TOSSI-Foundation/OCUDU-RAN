#include "ocudu/support/fapi_split_trace.h"

#include <algorithm>
#include <array>
#include <cerrno>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <mutex>
#include <string>
#include <sys/stat.h>
#include <sys/types.h>

namespace ocudu::fapi_split_trace {

namespace {

constexpr size_t kMaxPreviewBytes = 128;

std::mutex  g_mu;
FILE*       g_fp         = nullptr;
bool        g_enabled    = false;
std::string g_binary_tag;  // e.g. "odu_high" / "odu_low"

void format_timestamp(char* out, size_t cap)
{
  struct timespec ts;
  clock_gettime(CLOCK_REALTIME, &ts);
  struct tm tm_buf;
  localtime_r(&ts.tv_sec, &tm_buf);
  std::snprintf(out, cap, "%04d-%02d-%02d %02d:%02d:%02d.%09ld",
                tm_buf.tm_year + 1900,
                tm_buf.tm_mon + 1,
                tm_buf.tm_mday,
                tm_buf.tm_hour,
                tm_buf.tm_min,
                tm_buf.tm_sec,
                ts.tv_nsec);
}

void ensure_parent_dir(const std::string& path)
{
  size_t slash = path.find_last_of('/');
  if (slash == std::string::npos || slash == 0) {
    return;
  }
  std::string parent = path.substr(0, slash);

  size_t pos = 1;
  while (pos <= parent.size()) {
    size_t next = parent.find('/', pos);
    std::string segment = parent.substr(0, (next == std::string::npos) ? parent.size() : next);
    ::mkdir(segment.c_str(), 0755);  // ignore errors (EEXIST is fine)
    if (next == std::string::npos) {
      break;
    }
    pos = next + 1;
  }
}

} // namespace

void init(bool enabled, const std::string& path, const char* binary_tag)
{
  std::lock_guard<std::mutex> lock(g_mu);
  if (!enabled) {
    g_enabled = false;
    return;
  }
  ensure_parent_dir(path);
  g_fp = std::fopen(path.c_str(), "a");
  if (g_fp == nullptr) {
    std::fprintf(stderr,
                 "[fapi_split_trace] ERROR: could not open %s (%s) — tracing disabled\n",
                 path.c_str(), std::strerror(errno));
    g_enabled = false;
    return;
  }
  std::setvbuf(g_fp, nullptr, _IOLBF, 0);
  g_binary_tag = (binary_tag != nullptr) ? binary_tag : "?";
  g_enabled    = true;

  char ts[96];
  format_timestamp(ts, sizeof(ts));
  std::fprintf(g_fp, "\n==== [%s] [%s] fapi_split_trace OPEN ====\n", ts, g_binary_tag.c_str());
  std::fflush(g_fp);
}

bool is_enabled()
{
  return g_enabled;
}

void shutdown()
{
  std::lock_guard<std::mutex> lock(g_mu);
  if (g_fp == nullptr) {
    g_enabled = false;
    return;
  }
  char ts[96];
  format_timestamp(ts, sizeof(ts));
  std::fprintf(g_fp, "==== [%s] [%s] fapi_split_trace CLOSE ====\n\n", ts, g_binary_tag.c_str());
  std::fflush(g_fp);
  std::fclose(g_fp);
  g_fp      = nullptr;
  g_enabled = false;
}

void event(const char* component, const char* fmt, ...)
{
  if (!g_enabled) {
    return;
  }
  char ts[96];
  format_timestamp(ts, sizeof(ts));

  char body[1024];
  va_list ap;
  va_start(ap, fmt);
  std::vsnprintf(body, sizeof(body), fmt, ap);
  va_end(ap);

  std::lock_guard<std::mutex> lock(g_mu);
  if (g_fp == nullptr) {
    return;
  }
  std::fprintf(g_fp, "[%s] [%s] %s | %s\n", ts, g_binary_tag.c_str(), component, body);
}

void message(const char* component,
             const char* type,
             const char* direction,
             uint16_t    sfn,
             uint16_t    slot,
             size_t      total_size,
             const void* data,
             size_t      data_len)
{
  if (!g_enabled) {
    return;
  }
  char ts[96];
  format_timestamp(ts, sizeof(ts));

  std::array<char, kMaxPreviewBytes * 3 + 1> hex{};
  size_t preview = (data_len < kMaxPreviewBytes) ? data_len : kMaxPreviewBytes;
  if (data != nullptr && preview > 0) {
    const auto* p = static_cast<const uint8_t*>(data);
    char* w = hex.data();
    for (size_t i = 0; i < preview; ++i) {
      std::snprintf(w, 4, "%02X ", p[i]);
      w += 3;
    }
    if (w > hex.data()) {
      *(w - 1) = '\0';  // drop trailing space
    }
  }

  char sfn_slot[32];
  if (sfn == 0xFFFF && slot == 0xFFFF) {
    std::snprintf(sfn_slot, sizeof(sfn_slot), "sfn=- slot=-");
  } else if (slot == 0xFFFF) {
    std::snprintf(sfn_slot, sizeof(sfn_slot), "sfn=%u slot=-", sfn);
  } else {
    std::snprintf(sfn_slot, sizeof(sfn_slot), "sfn=%u slot=%u", sfn, slot);
  }

  std::lock_guard<std::mutex> lock(g_mu);
  if (g_fp == nullptr) {
    return;
  }
  std::fprintf(g_fp,
               "[%s] [%s] %s | type=%s dir=%s %s size=%zu preview_len=%zu data=[%s]\n",
               ts,
               g_binary_tag.c_str(),
               component,
               type,
               direction,
               sfn_slot,
               total_size,
               preview,
               hex.data());
}

void payload_full(const char* component,
                  const char* tag,
                  uint16_t    sfn,
                  uint16_t    slot,
                  uint16_t    rnti,
                  const void* data,
                  size_t      data_len)
{
  if (!g_enabled) {
    return;
  }

  char ts[96];
  format_timestamp(ts, sizeof(ts));

  std::lock_guard<std::mutex> lock(g_mu);
  if (g_fp == nullptr) {
    return;
  }

  std::fprintf(g_fp,
               "[%s] [%s] %s | %s sfn=%u slot=%u rnti=0x%04X size=%zu bytes=",
               ts,
               g_binary_tag.c_str(),
               component,
               tag,
               sfn,
               slot,
               rnti,
               data_len);

  if (data != nullptr && data_len > 0) {
    const auto* p = static_cast<const uint8_t*>(data);
    constexpr size_t stride = 128;
    char             chunk[stride * 2 + 1];
    for (size_t off = 0; off < data_len; off += stride) {
      size_t n = std::min<size_t>(stride, data_len - off);
      for (size_t i = 0; i < n; ++i) {
        std::snprintf(chunk + i * 2, 3, "%02x", p[off + i]);
      }
      std::fwrite(chunk, 1, n * 2, g_fp);
    }
  }
  std::fputc('\n', g_fp);
}

} // namespace ocudu::fapi_split_trace
