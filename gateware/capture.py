#
# This file is part of SLogic16U3.
#
# Copyright (c) 2026 Florent Kermarrec <florent@enjoy-digital.fr>
# SPDX-License-Identifier: BSD-2-Clause

"""Capture: samples (ratio samples per cycle) to a stream of packed data words.

Pipeline (sample clock domain, one input word of `ratio` samples every cycle):

    samples -> Source (inputs/test pattern) -> Decimation (/divider) -> Limit -> Width (4/8/16 channels)
            -> Packer (32-bit) -> Converter (data_width) -> source

- Decimation: one sample kept every `divider` samples (divider >= 1, base rate/divider), the first sample
  of the capture is kept. With divider < ratio several samples are kept per cycle, compacted.
- Width: channel mode 0/1/2 = 4/8/16 channels (CH0..CHn-1), n bits per sample on the stream (with 4
  channels: 2 samples per byte, first sample on the low nibble).
- Packer: up to 32 bits per cycle (4ch/divider 1, 8ch/divider 2, 16ch/divider 4 with ratio=8); more is
  a configuration error (`error`, capture stopped). The decimated lanes are only as wide as the valid
  configurations need (lanes 0-1: 16-bit, 2-3: 8-bit, 4-7: 4-bit).
- Limit: capture stopped after `limit` samples (0: until disabled), the last word is flushed (padded)
  with `last` set.
- Overflow: data word not accepted by the sink while capturing (sink not ready): capture stopped,
  `overflow` set (sticky until the next start), the last word flushed with `last`.

Test pattern (pattern=1): 16-bit counter incremented on each sample (sample i of the capture = i), so a
capture with divider D gives i*D (masked to the channels): end-to-end check without probes.
"""

from migen import *

from litex.gen import *

from litex.soc.interconnect import stream

# Constants ----------------------------------------------------------------------------------------

CAPTURE_MODE_4CH  = 0
CAPTURE_MODE_8CH  = 1
CAPTURE_MODE_16CH = 2
CAPTURE_MODES     = {CAPTURE_MODE_4CH: 4, CAPTURE_MODE_8CH: 8, CAPTURE_MODE_16CH: 16}

CAPTURE_PATTERN_INPUTS  = 0
CAPTURE_PATTERN_COUNTER = 1

CAPTURE_COUNT_BITS = 48 # Samples counters (~4 days at 800MS/s).

# Capture ------------------------------------------------------------------------------------------

class Capture(LiteXModule):
    def __init__(self, nchannels=16, ratio=8, data_width=64):
        assert nchannels == 16
        assert ratio == 8
        assert data_width in [32, 64]
        cbits = CAPTURE_COUNT_BITS
        self.ratio      = ratio
        self.data_width = data_width

        self.samples = Signal(ratio*nchannels) # i: Samples, first sample on the LSBs.
        self.source  = source = stream.Endpoint([("data", data_width)])

        # Control.
        self.enable  = Signal()      # i: Capture enable (rising: start, falling: stop).
        self.mode    = Signal(2)     # i: Channel mode (quasi-static).
        self.divider = Signal(32)    # i: Decimation (>= 1, quasi-static).
        self.pattern = Signal(2)     # i: Samples source (quasi-static).
        self.limit   = Signal(cbits) # i: Samples to capture (0: no limit, quasi-static).

        # Status.
        self.running  = Signal()      # o
        self.done     = Signal()      # o: Capture done (limit reached, stopped or overflow/error).
        self.overflow = Signal()      # o: Sink not ready while capturing (data lost).
        self.error    = Signal()      # o: More than 32 bits per cycle (invalid configuration).
        self.count    = Signal(cbits) # o: Captured samples.

        # # #

        # Control ----------------------------------------------------------------------------------
        enable_d = Signal()
        start    = Signal()
        self.sync += enable_d.eq(self.enable)
        self.comb += start.eq(self.enable & ~enable_d)
        divider = Signal(32)
        divsat  = Signal(4) # min(divider, ratio).
        div_big = Signal()  # divider >= ratio.
        self.sync += [      # Quasi-static, registered.
            divider.eq(Mux(self.divider == 0, 1, self.divider)),
            div_big.eq(divider >= ratio),
            divsat.eq(Mux(divider >= ratio, ratio, divider[:4])),
        ]

        # Source (inputs/test pattern) -------------------------------------------------------------
        # The word of the start cycle is the first captured one (s0_start), counter restarted at 0.
        counter   = Signal(16)
        counter_c = Signal(16)
        s0        = Signal(ratio*nchannels)
        s0_start  = Signal()
        s0_run    = Signal()
        self.comb += counter_c.eq(Mux(start, 0, counter))
        self.sync += [
            counter.eq(counter_c + ratio),
            Case(self.pattern, {
                # counter_c is a multiple of ratio: counter_c + i = Cat(i, counter_c[3:]).
                CAPTURE_PATTERN_COUNTER : s0.eq(Cat(*[Cat(Constant(i, 3), counter_c[3:]) for i in range(ratio)])),
                "default"               : s0.eq(self.samples),
            }),
            s0_start.eq(start),
            s0_run.eq(self.enable),
        ]

        # Decimation -------------------------------------------------------------------------------
        # skip: samples to skip before the next kept one, from the first sample of the current word.
        # Positions of the kept samples: skip + j*divsat (j < ratio, kept while < ratio).
        skip     = Signal(32)
        skip_cur = Signal(32)
        skip_n   = Signal(32)
        skip_lo  = Signal(4)
        pos      = [Signal(7) for _ in range(ratio + 1)]
        valid    = Signal(ratio)
        k        = Signal(4)
        self.comb += [
            skip_cur.eq(Mux(s0_start, 0, skip)),
            skip_lo.eq(Mux(skip_cur >= ratio, ratio, skip_cur[:4])),
            [pos[j].eq(skip_lo + j*divsat) for j in range(ratio + 1)],
            [valid[j].eq(pos[j] < ratio) for j in range(ratio)],
            k.eq(Reduce("ADD", [valid[j] for j in range(ratio)])),
            If(skip_cur >= ratio,
                skip_n.eq(skip_cur - ratio)
            ).Elif(div_big,
                skip_n.eq(skip_cur + divider - ratio)
            ).Else(
                Case(k, {n: skip_n.eq(pos[n] - ratio) for n in range(ratio + 1)})
            )
        ]

        # Decimated samples (compacted: d1[0:d1_k] valid), registered, lanes sized for the valid
        # configurations.
        lane_bits = [16, 16, 8, 8, 4, 4, 4, 4]
        d1        = [Signal(lane_bits[j]) for j in range(ratio)]
        d1_k      = Signal(4)
        d1_run    = Signal()
        for j in range(ratio):
            choices = Array([s0[nchannels*i:nchannels*i + lane_bits[j]] for i in range(ratio)])
            self.sync += d1[j].eq(choices[pos[j][:3]])
        self.sync += [
            If(s0_run, skip.eq(skip_n)),
            d1_k.eq(Mux(s0_run, k, 0)),
            d1_run.eq(s0_run),
        ]

        # Limit ------------------------------------------------------------------------------------
        # remaining: samples left (down counter), rem_small: remaining < 16 (registered).
        remaining = Signal(cbits)
        rem_small = Signal()
        limit_en  = Signal()
        d2_k      = Signal(4)
        limited   = Signal()
        self.comb += [
            If(limit_en & rem_small & (remaining[:5] <= d1_k),
                d2_k.eq(remaining[:4]),
                limited.eq(1),
            ).Else(
                d2_k.eq(d1_k),
            )
        ]

        # Width (registered) -----------------------------------------------------------------------
        # Chunk: d2_k samples of w bits, contiguous from the LSBs.
        chunk      = Signal(32)
        chunk_bits = Signal(6)
        too_wide   = Signal()
        d3_run     = Signal()
        d3_limited = Signal()
        chunks = {
            CAPTURE_MODE_4CH  : Cat(*[d1[j][:4] for j in range(8)]),
            CAPTURE_MODE_8CH  : Cat(*[d1[j][:8] for j in range(4)]),
            CAPTURE_MODE_16CH : Cat(*[d1[j][:16] for j in range(2)]),
        }
        nbits = Signal(8)
        mask  = Signal(32)
        self.comb += [
            Case(self.mode, {
                CAPTURE_MODE_4CH  : nbits.eq(d2_k*4),
                CAPTURE_MODE_8CH  : nbits.eq(d2_k*8),
                "default"         : nbits.eq(d2_k*16),
            }),
            If(nbits >= 32,
                mask.eq(2**32 - 1)
            ).Else(
                Case(nbits[:5], {n: mask.eq(2**n - 1) for n in range(32)})
            ),
        ]
        self.sync += [
            Case(self.mode, {
                CAPTURE_MODE_4CH  : chunk.eq(chunks[CAPTURE_MODE_4CH]  & mask),
                CAPTURE_MODE_8CH  : chunk.eq(chunks[CAPTURE_MODE_8CH]  & mask),
                "default"         : chunk.eq(chunks[CAPTURE_MODE_16CH] & mask),
            }),
            chunk_bits.eq(nbits),
            too_wide.eq(nbits > 32),
            d3_run.eq(d1_run),
            d3_limited.eq(limited),
        ]

        # Packer (32-bit) --------------------------------------------------------------------------
        buf     = Signal(64)
        level   = Signal(6) # Bits in buf (< 32 between cycles, multiple of 4).
        buf_n   = Signal(64)
        level_n = Signal(7)
        shifted = Signal(64)
        self.comb += [
            Case(level[2:5], {n: shifted.eq(chunk << 4*n) for n in range(8)}),
            buf_n.eq(buf | shifted),
            level_n.eq(level + chunk_bits),
        ]

        # Converter (32-bit -> data_width).
        self.conv = conv = stream.Converter(32, data_width)
        self.comb += conv.source.connect(source, omit={"valid_token_count"})
        sink = conv.sink

        # FSM --------------------------------------------------------------------------------------
        emit     = Signal()
        stop_req = Signal() # Limit reached (no more samples counted).
        d3_seen  = Signal() # Width stage data seen (end of the capture: d3_run falling).
        self.fsm = fsm = FSM(reset_state="IDLE")
        # Start: counters/packer cleared, the first decimated word reaches the limit stage on the
        # first RUN cycle and the packer one cycle later.
        fsm.act("IDLE",
            If(s0_start,
                NextValue(self.overflow, 0),
                NextValue(self.error,    0),
                NextValue(self.done,     0),
                NextValue(self.count,    0),
                NextValue(remaining, self.limit),
                NextValue(rem_small, self.limit < 16),
                NextValue(limit_en,  self.limit != 0),
                NextValue(stop_req, 0),
                NextValue(d3_seen,  0),
                NextValue(buf,   0),
                NextValue(level, 0),
                NextState("RUN")
            )
        )
        fsm.act("RUN",
            self.running.eq(1),
            # Limit stage (d1).
            If(d1_run & ~stop_req,
                NextValue(self.count, self.count + d2_k),
                NextValue(remaining, remaining - d2_k),
                NextValue(rem_small, (remaining - d2_k) < 16),
                If(limited, NextValue(stop_req, 1))
            ),
            # Packer (width stage outputs).
            If(d3_run,
                NextValue(d3_seen, 1),
                If(level_n >= 32,
                    emit.eq(1),
                    sink.valid.eq(1),
                    sink.data.eq(buf_n[:32]),
                    NextValue(buf,   buf_n[32:]),
                    NextValue(level, level_n - 32),
                ).Else(
                    NextValue(buf,   buf_n),
                    NextValue(level, level_n),
                )
            ),
            If(d3_run & too_wide,
                NextValue(self.error, 1),
                NextState("FLUSH")
            ).Elif(emit & ~sink.ready,
                NextValue(self.overflow, 1),
                NextState("FLUSH")
            ).Elif(d3_run & d3_limited,
                NextState("FLUSH")
            ).Elif(d3_seen & ~d3_run,
                NextState("FLUSH")
            )
        )
        # Flush: remaining bits (padded) with last (an empty last word when aligned: the stream end is
        # always marked).
        fsm.act("FLUSH",
            sink.valid.eq(1),
            sink.data.eq(buf[:32]),
            sink.last.eq(1),
            If(sink.ready,
                NextValue(self.done, 1),
                NextState("IDLE")
            )
        )
