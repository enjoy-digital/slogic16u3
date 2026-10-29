#
# This file is part of SLogic16U3.
#
# Copyright (c) 2026 Florent Kermarrec <florent@enjoy-digital.fr>
# SPDX-License-Identifier: BSD-2-Clause

"""Capture: samples (ratio samples per cycle) to a stream of packed data words.

Pipeline (sample clock domain, one input word of `ratio` samples every cycle):

    samples -> Source (inputs/test pattern) -> Decimation (/divider) -> Width (4/8/16 channels)
            -> Packer (data_width-bit words, sample-major, first sample on the LSBs) -> source

- Decimation: one sample kept every `divider` samples (divider >= 1, base rate/divider), the first sample
  of the capture is kept. With divider < ratio several samples are kept per cycle, compacted.
- Width: channel mode 0/1/2 = 4/8/16 channels (CH0..CHn-1), n bits per sample on the stream (with 4
  channels: 2 samples per byte, first sample on the low nibble).
- Packer: up to 32 bits per cycle (ex: 4ch/divider 1, 8ch/divider 2, 16ch/divider 4 with ratio=8), more
  is a configuration error (`error`, capture stopped).
- Limit: capture stopped after `limit` samples (0: until disabled), the last word is flushed (padded)
  with `last` set.
- Overflow: data word not accepted by the sink while capturing (sink not ready): capture stopped,
  `overflow` set (sticky until the next start), the last word flushed with `last`.

Test pattern (pattern=1): 16-bit counter incremented on each sample (sample i of the capture = i), so a
capture with divider D gives i*D (masked to the channels): end-to-end check without probes.
"""

from migen import *

from litex.gen import *

from litex.soc.interconnect.csr import *
from litex.soc.interconnect     import stream

# Constants ----------------------------------------------------------------------------------------

CAPTURE_MODE_4CH  = 0
CAPTURE_MODE_8CH  = 1
CAPTURE_MODE_16CH = 2
CAPTURE_MODES     = {CAPTURE_MODE_4CH: 4, CAPTURE_MODE_8CH: 8, CAPTURE_MODE_16CH: 16}

CAPTURE_PATTERN_INPUTS  = 0
CAPTURE_PATTERN_COUNTER = 1

# Capture ------------------------------------------------------------------------------------------

class Capture(LiteXModule):
    def __init__(self, nchannels=16, ratio=8, data_width=64):
        assert nchannels == 16
        assert ratio in [1, 2, 4, 8, 16]
        assert data_width in [32, 64]
        self.ratio      = ratio
        self.data_width = data_width

        self.samples = Signal(ratio*nchannels) # i: Samples, first sample on the LSBs.
        self.source  = source = stream.Endpoint([("data", data_width)])

        # Control.
        self.enable  = Signal()   # i: Capture enable (rising: start, falling: stop).
        self.mode    = Signal(2)  # i: Channel mode (quasi-static).
        self.divider = Signal(32) # i: Decimation (>= 1, quasi-static).
        self.pattern = Signal(2)  # i: Samples source (quasi-static).
        self.limit   = Signal(64) # i: Samples to capture (0: no limit, quasi-static).

        # Status.
        self.running  = Signal()   # o
        self.done     = Signal()   # o: Capture done (limit reached, stopped or overflow/error).
        self.overflow = Signal()   # o: Sink not ready while capturing (data lost).
        self.error    = Signal()   # o: More than 32 bits per cycle (invalid configuration).
        self.count    = Signal(64) # o: Captured samples.
        self.words    = Signal(64) # o: Data words sent.

        # # #

        # Control ----------------------------------------------------------------------------------
        enable_d = Signal()
        start    = Signal()
        stop     = Signal()
        self.sync += enable_d.eq(self.enable)
        self.comb += [
            start.eq( self.enable & ~enable_d),
            stop.eq( ~self.enable &  enable_d),
        ]
        divider = Signal(32)
        self.comb += divider.eq(Mux(self.divider == 0, 1, self.divider))

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
                CAPTURE_PATTERN_COUNTER : s0.eq(Cat(*[(counter_c + i)[:16] for i in range(ratio)])),
                "default"               : s0.eq(self.samples),
            }),
            s0_start.eq(start),
            s0_run.eq(self.enable),
        ]

        # Decimation -------------------------------------------------------------------------------
        # skip: samples to skip before the next kept one, from the first sample of the current word.
        skip     = Signal(32)
        skip_cur = Signal(32)
        skip_n   = Signal(32)
        divsat   = Signal(max=ratio + 1) # min(divider, ratio).
        pos      = [Signal(max=ratio*(ratio + 2)) for _ in range(ratio + 1)] # skip + j*divsat.
        valid    = Signal(ratio)
        k        = Signal(max=ratio + 1)
        self.comb += [
            skip_cur.eq(Mux(s0_start, 0, skip)),
            divsat.eq(Mux(divider >= ratio, ratio, divider)),
            [pos[j].eq(skip_cur[:bits_for(ratio)] + j*divsat) for j in range(ratio + 1)],
            [valid[j].eq((skip_cur < ratio) & (pos[j] < ratio)) for j in range(ratio)],
            k.eq(Reduce("ADD", [valid[j] for j in range(ratio)])),
            If(skip_cur >= ratio,
                skip_n.eq(skip_cur - ratio)
            ).Elif(divider >= ratio,
                skip_n.eq(skip_cur + divider - ratio)
            ).Else(
                Case(k, {n: skip_n.eq(pos[n] - ratio) for n in range(ratio + 1)})
            )
        ]

        # Decimated samples (compacted: d1[0:d1_k] valid), registered.
        d1      = [Signal(nchannels) for _ in range(ratio)]
        d1_k    = Signal(max=ratio + 1)
        d1_run  = Signal()
        samples = Array([s0[nchannels*i:nchannels*(i + 1)] for i in range(ratio)])
        self.sync += [
            If(s0_run, skip.eq(skip_n)),
            [d1[j].eq(samples[pos[j][:bits_for(ratio - 1)]]) for j in range(ratio)],
            d1_k.eq(Mux(s0_run, k, 0)),
            d1_run.eq(s0_run),
        ]
        # Limit ------------------------------------------------------------------------------------
        remaining = Signal(64)
        d2_k      = Signal(max=ratio + 1)
        limited   = Signal()
        self.comb += [
            remaining.eq(self.limit - self.count),
            If((self.limit != 0) & (remaining < d1_k),
                d2_k.eq(remaining),
                limited.eq(1),
            ).Elif((self.limit != 0) & (remaining == d1_k),
                d2_k.eq(d1_k),
                limited.eq(1),
            ).Else(
                d2_k.eq(d1_k),
            )
        ]

        # Width ------------------------------------------------------------------------------------
        # Chunk: d2_k samples of w bits, contiguous from the LSBs (up to ratio*16 bits).
        chunk_bits = Signal(max=ratio*nchannels + 1)
        chunk      = Signal(32)
        too_wide   = Signal()
        chunks     = {
            CAPTURE_MODE_4CH  : Cat(*[d1[j][:4]  for j in range(ratio)]),
            CAPTURE_MODE_8CH  : Cat(*[d1[j][:8]  for j in range(ratio)]),
            CAPTURE_MODE_16CH : Cat(*[d1[j][:16] for j in range(ratio)]),
        }
        self.comb += [
            Case(self.mode, {
                CAPTURE_MODE_4CH  : chunk_bits.eq(d2_k*4),
                CAPTURE_MODE_8CH  : chunk_bits.eq(d2_k*8),
                "default"         : chunk_bits.eq(d2_k*16),
            }),
            Case(self.mode, {
                CAPTURE_MODE_4CH  : chunk.eq(chunks[CAPTURE_MODE_4CH]),
                CAPTURE_MODE_8CH  : chunk.eq(chunks[CAPTURE_MODE_8CH]),
                "default"         : chunk.eq(chunks[CAPTURE_MODE_16CH]),
            }),
            too_wide.eq(chunk_bits > 32),
        ]
        chunk_mask = Signal(32)
        self.comb += Case(chunk_bits, {n: chunk_mask.eq(2**n - 1) for n in range(33)})

        # Packer -----------------------------------------------------------------------------------
        dw      = data_width
        buf     = Signal(dw + 32)
        level   = Signal(max=dw + 33)
        buf_n   = Signal(dw + 32)
        level_n = Signal(max=dw + 33)
        shifted = Signal(dw + 32)
        self.comb += [
            Case(level, {n: shifted.eq((chunk & chunk_mask) << n) for n in range(0, dw, 4)}),
            buf_n.eq(buf | shifted),
            level_n.eq(level + chunk_bits),
        ]

        # FSM --------------------------------------------------------------------------------------
        word      = Signal(dw)
        word_last = Signal()
        emit      = Signal()
        self.fsm = fsm = FSM(reset_state="IDLE")
        fsm.act("IDLE",
            If(s0_start,
                NextValue(self.overflow, 0),
                NextValue(self.error,    0),
                NextValue(self.done,     0),
                NextValue(self.count,    0),
                NextValue(self.words,    0),
                NextValue(buf,   0),
                NextValue(level, 0),
                NextState("RUN")
            )
        )
        fsm.act("RUN",
            self.running.eq(1),
            If(too_wide,
                NextValue(self.error, 1),
                NextState("FLUSH")
            ).Else(
                NextValue(self.count, self.count + d2_k),
                If(level_n >= dw,
                    emit.eq(1),
                    source.valid.eq(1),
                    source.data.eq(buf_n[:dw]),
                    If(~source.ready,
                        NextValue(self.overflow, 1),
                    ),
                    NextValue(buf,   buf_n[dw:]),
                    NextValue(level, level_n - dw),
                ).Else(
                    NextValue(buf,   buf_n),
                    NextValue(level, level_n),
                ),
                If(emit & ~source.ready,
                    NextState("FLUSH")
                ).Elif(limited | ~d1_run,
                    NextState("FLUSH")
                )
            )
        )
        # Flush: remaining bits (padded) with last, or an empty last word when aligned (the stream
        # end is always marked).
        fsm.act("FLUSH",
            source.valid.eq(1),
            source.data.eq(buf[:dw]),
            source.last.eq(1),
            If(source.ready,
                NextValue(self.words, self.words + 1),
                NextValue(self.done, 1),
                NextState("IDLE")
            )
        )
        self.sync += If(fsm.ongoing("RUN") & emit & source.ready, self.words.eq(self.words + 1))

    def add_csr(self):
        self._control = CSRStorage(fields=[
            CSRField("enable",  size=1, offset=0, description="Capture enable (rising: start, falling: stop)."),
            CSRField("mode",    size=2, offset=4, values=[
                ("``0b00``", "4 channels (CH0-CH3)."),
                ("``0b01``", "8 channels (CH0-CH7)."),
                ("``0b10``", "16 channels (CH0-CH15)."),
            ]),
            CSRField("pattern", size=2, offset=8, values=[
                ("``0b00``", "Inputs."),
                ("``0b01``", "Counter (test pattern)."),
            ]),
        ])
        self._divider = CSRStorage(32, reset=1, description="Decimation (samplerate = base/divider).")
        self._limit   = CSRStorage(64, description="Samples to capture (0: until disabled).")
        self._status  = CSRStatus(fields=[
            CSRField("running",  size=1, offset=0),
            CSRField("done",     size=1, offset=1),
            CSRField("overflow", size=1, offset=2, description="Data lost (host too slow): capture stopped."),
            CSRField("error",    size=1, offset=3, description="Invalid configuration (> 32 bits/cycle)."),
        ])
        self._count = CSRStatus(64, description="Captured samples.")
        self._words = CSRStatus(64, description="Data words sent.")
        self.comb += [
            self.enable.eq(self._control.fields.enable),
            self.mode.eq(self._control.fields.mode),
            self.pattern.eq(self._control.fields.pattern),
            self.divider.eq(self._divider.storage),
            self.limit.eq(self._limit.storage),
            self._status.fields.running.eq(self.running),
            self._status.fields.done.eq(self.done),
            self._status.fields.overflow.eq(self.overflow),
            self._status.fields.error.eq(self.error),
            self._count.status.eq(self.count),
            self._words.status.eq(self.words),
        ]
