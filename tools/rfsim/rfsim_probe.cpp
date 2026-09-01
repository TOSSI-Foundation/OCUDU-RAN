

#include <arpa/inet.h>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
#include <vector>
#include <ctime>

struct hdr_t {
  uint32_t size;
  uint32_t nbAnt;
  uint64_t timestamp;
  uint32_t option_value;
  uint32_t option_flag;
  uint64_t beam_map;
};
struct c16 {
  int16_t r, i;
};

static void read_exact(int fd, void* buf, size_t n)
{
  char* p = (char*)buf;
  while (n) {
    ssize_t r = read(fd, p, n);
    if (r <= 0) {
      fprintf(stderr, "read failed\n");
      exit(1);
    }
    p += r;
    n -= r;
  }
}

int main(int argc, char** argv)
{
  int port   = (argc > 1) ? atoi(argv[1]) : 4043;
  int    blocks = (argc > 2) ? atoi(argv[2]) : 400;
  double secs   = (argc > 3) ? atof(argv[3]) : 0.0;

  int         fd = socket(AF_INET, SOCK_STREAM, 0);
  sockaddr_in sa = {};
  sa.sin_family  = AF_INET;
  sa.sin_port    = htons(port);
  inet_pton(AF_INET, "127.0.0.1", &sa.sin_addr);
  if (connect(fd, (sockaddr*)&sa, sizeof(sa)) != 0) {
    fprintf(stderr, "connect failed\n");
    return 1;
  }

  hdr_t hello = {};
  read_exact(fd, &hello, sizeof(hello));
  std::vector<c16> tmp(hello.size * hello.nbAnt);
  read_exact(fd, tmp.data(), tmp.size() * sizeof(c16));

  double   sumsq = 0;
  uint64_t n     = 0;
  int      peak = 0, nonzero = 0;

  timespec tb;
  clock_gettime(CLOCK_MONOTONIC, &tb);
  auto elapsed = [&]() {
    timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (t.tv_sec - tb.tv_sec) + 1e-9 * (t.tv_nsec - tb.tv_nsec);
  };

  for (int b = 0; b < blocks; ++b) {
    if (secs > 0 && elapsed() >= secs) {
      break;
    }
    hdr_t dl = {};
    read_exact(fd, &dl, sizeof(dl));
    std::vector<c16> s(dl.size * dl.nbAnt);
    read_exact(fd, s.data(), s.size() * sizeof(c16));

    for (auto& v : s) {
      sumsq += (double)v.r * v.r + (double)v.i * v.i;
      ++n;
      int m = std::max(std::abs((int)v.r), std::abs((int)v.i));
      if (m > peak) {
        peak = m;
      }
      if (m) {
        ++nonzero;
      }
    }

    std::vector<c16> ul(dl.size, {0, 0});
    hdr_t            up = {dl.size, 1, dl.timestamp, 0, 0, 1};
    if (write(fd, &up, sizeof(up)) != sizeof(up)) {
      break;
    }
    if (write(fd, ul.data(), ul.size() * sizeof(c16)) < 0) {
      break;
    }
  }

  double rms = std::sqrt(sumsq / n);
  printf("elapsed=%.3f  samples=%lu  rms=%.1f (%.1f dBFS)  peak=%d  nonzero=%.1f%%\n",
         elapsed(),
         n,
         rms,
         20 * log10(rms / 32767.0 + 1e-12),
         peak,
         100.0 * nonzero / n);
  close(fd);
  return 0;
}
