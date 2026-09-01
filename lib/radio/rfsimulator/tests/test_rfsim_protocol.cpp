// SPDX-FileCopyrightText: Copyright (C) 2021-2026 Software Radio Systems Limited
// SPDX-License-Identifier: BSD-3-Clause-Open-MPI

#include <arpa/inet.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <netinet/in.h>
#include <sys/socket.h>
#include <thread>
#include <unistd.h>
#include <string>
#include <vector>

#include "rfsim_shim.h"
#include "common_lib.h"

#define CHECK(cond)                                                                                \
  do {                                                                                             \
    if (!(cond)) {                                                                                 \
      fprintf(stderr, "%s:%d: check failed: %s\n", __FILE__, __LINE__, #cond);                     \
      std::abort();                                                                                \
    }                                                                                              \
  } while (0)

static constexpr int      PORT   = 43217;
static constexpr uint32_t NSAMPS = 256;
static constexpr int      NBLOCK = 4;

static_assert(sizeof(samplesBlockHeader_t) == 32, "rfsimulator header is 32 bytes on the wire");
static_assert(sizeof(c16_t) == 4, "rfsimulator sample is 4 bytes on the wire");

static void read_exact(int fd, void *buf, size_t n)
{
  char *p = static_cast<char *>(buf);
  while (n) {
    ssize_t r = read(fd, p, n);
    CHECK(r > 0 && "client read");
    p += r;
    n -= r;
  }
}

static c16_t pattern(int block, uint32_t i)
{
  int16_t v = static_cast<int16_t>(block * 1000 + i);
  return {v, static_cast<int16_t>(-v)};
}

static void fake_ue()
{
  int fd = socket(AF_INET, SOCK_STREAM, 0);
  CHECK(fd >= 0);
  sockaddr_in sa = {};
  sa.sin_family  = AF_INET;
  sa.sin_port    = htons(PORT);
  inet_pton(AF_INET, "127.0.0.1", &sa.sin_addr);
  while (connect(fd, reinterpret_cast<sockaddr *>(&sa), sizeof(sa)) != 0) {
    usleep(1000);
  }

  samplesBlockHeader_t hello = {};
  read_exact(fd, &hello, sizeof(hello));
  CHECK(hello.size == 1 && "hello block is one sample");
  CHECK(hello.nbAnt == 1);
  CHECK(hello.beam_map == 1 && "single beam");
  c16_t discard;
  read_exact(fd, &discard, sizeof(discard));

  for (int b = 0; b < NBLOCK; ++b) {
    samplesBlockHeader_t dl = {};
    read_exact(fd, &dl, sizeof(dl));
    CHECK(dl.size == NSAMPS && "downlink block size");
    std::vector<c16_t> dl_samples(dl.size * dl.nbAnt);
    read_exact(fd, dl_samples.data(), dl_samples.size() * sizeof(c16_t));

    std::vector<c16_t> ul(NSAMPS);
    for (uint32_t i = 0; i != NSAMPS; ++i) {
      ul[i] = pattern(b, i);
    }
    samplesBlockHeader_t up = {NSAMPS, 1, dl.timestamp, 0, 0, 1};
    CHECK(write(fd, &up, sizeof(up)) == sizeof(up));
    CHECK(write(fd, ul.data(), ul.size() * sizeof(c16_t)) == (ssize_t)(ul.size() * sizeof(c16_t)));
  }
  close(fd);
}

int main()
{
  ocudu::rfsim_config_set("serveraddr", "server");
  ocudu::rfsim_config_set("serverport", std::to_string(PORT).c_str());

  openair0_config_t cfg = {};
  cfg.ru_id             = 0;
  cfg.tx_num_channels   = 1;
  cfg.rx_num_channels   = 1;
  cfg.sample_rate       = 1e6;
  cfg.rx_freq[0]        = 3.5e9;
  cfg.tx_bw             = 1e6;

  openair0_device_t dev = {};
  CHECK(device_init(&dev, &cfg) == 0);
  CHECK(dev.trx_start_func(&dev) == 0);

  std::thread ue(fake_ue);

  std::vector<c16_t> txbuf(NSAMPS), rxbuf(NSAMPS);
  void              *txp = txbuf.data();
  void              *rxp = rxbuf.data();

  openair0_timestamp_t ts = 0;
  CHECK(dev.trx_read_func(&dev, &ts, &rxp, NSAMPS, 1) == (int)NSAMPS);

  int matched = 0;
  for (int b = 0; b < NBLOCK; ++b) {
    CHECK(dev.trx_write_func(&dev, b * NSAMPS, &txp, NSAMPS, 1, TX_BURST_START_AND_END) == (int)NSAMPS);

    if (b == 0) {
      continue;
    }
    CHECK(dev.trx_read_func(&dev, &ts, &rxp, NSAMPS, 1) == (int)NSAMPS);
    for (int k = 0; k < NBLOCK; ++k) {
      if (rxbuf[0].r == pattern(k, 0).r && rxbuf[NSAMPS - 1].r == pattern(k, NSAMPS - 1).r) {
        for (uint32_t i = 0; i != NSAMPS; ++i) {
          CHECK(rxbuf[i].r == pattern(k, i).r && rxbuf[i].i == pattern(k, i).i);
        }
        ++matched;
        break;
      }
    }
  }

  ue.join();
  CHECK(matched > 0 && "server never delivered an uplink block to the stack");
  printf("ok: header 32B, hello handshake, %d uplink block(s) round-tripped\n", matched);
  return 0;
}
