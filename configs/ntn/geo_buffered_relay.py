#!/usr/bin/env python3

# SPDX-FileCopyrightText: Copyright (C) 2021-2026 Software Radio Systems Limited
# SPDX-License-Identifier: BSD-3-Clause-Open-MPI

"""GEO NTN delay relay for the gNB <-> UE ZMQ link.

Applies a fixed one-way propagation delay in each direction so the link behaves
like the path the SIB19 ephemeris describes. Two threads per direction: a serve
loop paced by the consumer's requests, and a puller that continuously drains the
source. The FIFO prefill is the delay.

  DL: pull gNB TX tcp://127.0.0.1:2000  ->  serve UE RX  tcp://127.0.0.1:2100
  UL: pull UE TX  tcp://127.0.0.1:2101  ->  serve gNB RX tcp://127.0.0.1:2001

Requires pyzmq.
"""

import argparse
import threading
import time

import zmq

SAMPLE_BYTES = 8  # cf32 = 2 x float32


class Direction:
    def __init__(self, ctx, name, src_addr, dst_addr, delay_bytes, chunk_bytes,
                 margin_blocks, credit_blocks, underrun_timeout):
        self.name = name
        self.chunk_bytes = chunk_bytes
        self.zero = bytes(chunk_bytes)
        # Prefill == the delay: output[k] = input[k - delay_bytes].
        self.buf = bytearray(delay_bytes)
        self.read_off = 0
        self.delay_bytes = delay_bytes
        self.max_bytes = delay_bytes + margin_blocks * chunk_bytes
        self.underrun_timeout = underrun_timeout
        self.cond = threading.Condition()
        # Gating the serve on the puller pins served <= pulled, so level >= delay_bytes.
        self.serve_credit = 0
        self.credit_slack = credit_blocks * chunk_bytes
        self.primed = False
        self.src = ctx.socket(zmq.REQ)
        self.src.setsockopt(zmq.LINGER, 0)
        self.src.connect(src_addr)
        self.dst = ctx.socket(zmq.REP)
        self.dst.setsockopt(zmq.LINGER, 0)
        self.dst.bind(dst_addr)
        self.served = 0
        self.pulled = 0
        self.underruns = 0
        self.dropped = 0
        self.base_served = 0
        self.base_pulled = 0
        self.depth = delay_bytes

    def _level(self):
        return len(self.buf) - self.read_off

    def _reset_to_delay(self):
        """Snap the FIFO to exactly delay_bytes. Caller must hold self.cond."""
        self.serve_credit = 0
        self.dropped = 0
        self.base_served = self.served
        self.base_pulled = self.pulled
        level = self._level()
        if level > self.delay_bytes:
            self.read_off += level - self.delay_bytes
            del self.buf[: self.read_off]
            self.read_off = 0
        elif level < self.delay_bytes:
            self.buf = bytearray(self.delay_bytes - level) + self.buf[self.read_off :]
            self.read_off = 0
        self.depth = self._level()

    def serve_loop(self):
        """One block per consumer request. Never touches the source socket."""
        cb = self.chunk_bytes
        while True:
            self.dst.recv()
            with self.cond:
                if not self.primed:
                    # Whatever accumulated while the consumer was absent is stale.
                    self._reset_to_delay()
                    self.primed = True
                # Must wait, never pad: a pad that does not consume shifts the delay a slot.
                ready = self.serve_credit >= cb or self.cond.wait_for(
                    lambda: self.serve_credit >= cb, self.underrun_timeout)
                if not ready:
                    self.underruns += 1
                if self._level() >= cb:
                    out = bytes(self.buf[self.read_off : self.read_off + cb])
                    self.read_off += cb
                    if self.read_off >= (1 << 22):
                        del self.buf[: self.read_off]
                        self.read_off = 0
                    self.serve_credit -= cb
                else:
                    # Consume as well as pad so the stream offset stays intact.
                    out = self.zero
                    if self._level():
                        drop = min(self._level(), cb)
                        self.read_off += drop
                        self.serve_credit -= drop
                self.depth = self._level()
            self.dst.send(out)
            self.served += len(out)

    def pull_loop(self):
        """Drain the source continuously so the far end's TX buffer never backs up."""
        while True:
            self.src.send(b"\x00")
            data = self.src.recv()
            if not data:
                time.sleep(0.0005)
                continue
            with self.cond:
                self.buf.extend(data)
                self.serve_credit += len(data)
                excess = self._level() - self.max_bytes
                if excess > 0:
                    self.read_off += excess
                    self.dropped += excess
                    self.serve_credit -= excess
                if self.read_off >= (1 << 22):
                    del self.buf[: self.read_off]
                    self.read_off = 0
                self.depth = self._level()
                self.cond.notify_all()
            self.pulled += len(data)

    def start(self):
        for fn, suffix in ((self.serve_loop, "srv"), (self.pull_loop, "pull")):
            threading.Thread(target=fn, daemon=True, name="%s-%s" % (self.name, suffix)).start()


def main():
    p = argparse.ArgumentParser(description="Pull-on-serve GEO NTN ZMQ delay relay")
    p.add_argument("--channel-delay-us", type=float, default=120350.0)
    p.add_argument("--samp-rate", type=float, default=15.36e6)
    p.add_argument("--slot-us", type=float, default=1000.0, help="block size (1000 = one mu=0 slot)")
    p.add_argument("--margin-blocks", type=int, default=80,
                   help="FIFO headroom above the delay depth before the oldest samples are dropped. "
                        "Overflow backstop only; a non-zero 'drop' in the readout means a consumer "
                        "has stopped consuming.")
    p.add_argument("--credit-blocks", type=int, default=2,
                   help="how far the puller may run ahead of the consumer. Bounds the jitter on the "
                        "propagation delay, so keep it small.")
    p.add_argument("--underrun-timeout", type=float, default=None,
                   help="seconds a serve waits for real data. Default: wait indefinitely, which is what "
                        "keeps the stream aligned. A finite value makes the relay pad on stall, and every "
                        "pad shifts the delay by one slot.")
    args = p.parse_args()

    chunk_samples = int(round(args.samp_rate * args.slot_us / 1e6))
    chunk_bytes = chunk_samples * SAMPLE_BYTES
    delay_samples = int(round(args.samp_rate * args.channel_delay_us / 1e6))
    delay_bytes = delay_samples * SAMPLE_BYTES
    print("pull-on-serve relay: %.3f ms delay = %d samples, block %d samples, %.3f Msps"
          % (delay_samples / args.samp_rate * 1e3, delay_samples, chunk_samples, args.samp_rate / 1e6))

    ctx = zmq.Context(io_threads=4)
    dl = Direction(ctx, "DL", "tcp://127.0.0.1:2000", "tcp://127.0.0.1:2100",
                   delay_bytes, chunk_bytes, args.margin_blocks, args.credit_blocks,
                   args.underrun_timeout)
    ul = Direction(ctx, "UL", "tcp://127.0.0.1:2101", "tcp://127.0.0.1:2001",
                   delay_bytes, chunk_bytes, args.margin_blocks, args.credit_blocks,
                   args.underrun_timeout)
    dl.start()
    ul.start()
    print("relay running: DL 2000->2100, UL 2101->2001. Ctrl-C to stop.")
    print("healthy steady state: served ~= pulled in both directions, delay ~= %d smp, shift 0.\n"
          % delay_samples)

    last = [0, 0, 0, 0]
    while True:
        time.sleep(2.0)
        rates = [(now - was) / SAMPLE_BYTES / 2.0 / 1e6
                 for now, was in zip((dl.served, dl.pulled, ul.served, ul.pulled), last)]
        last = [dl.served, dl.pulled, ul.served, ul.pulled]

        def shift_smp(d):
            """Stream misalignment in samples. Must stay 0."""
            if not d.primed:
                return 0
            return ((d.served - d.base_served)
                    - ((d.pulled - d.base_pulled) + d.delay_bytes - d.depth - d.dropped)) // SAMPLE_BYTES

        dl_smp, ul_smp = dl.depth // SAMPLE_BYTES, ul.depth // SAMPLE_BYTES
        print("DL srv %.3f/pull %.3f Msps (delay %7d smp = %8.1f us, shift %+d, under %d, drop %d) | "
              "UL srv %.3f/pull %.3f Msps (delay %7d smp = %8.1f us, shift %+d, under %d, drop %d)"
              % (rates[0], rates[1], dl_smp, dl_smp / args.samp_rate * 1e6,
                 shift_smp(dl), dl.underruns, dl.dropped // SAMPLE_BYTES,
                 rates[2], rates[3], ul_smp, ul_smp / args.samp_rate * 1e6,
                 shift_smp(ul), ul.underruns, ul.dropped // SAMPLE_BYTES))


if __name__ == "__main__":
    main()
