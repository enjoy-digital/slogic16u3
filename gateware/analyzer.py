#
# This file is part of SLogic16U3.
#
# Copyright (c) 2026 Florent Kermarrec <florent@enjoy-digital.fr>
# SPDX-License-Identifier: BSD-2-Clause

"""Analyzer: Capture (sample clock domain) to the USB3 data stream (sys clock domain), CSRs.

    samples -> Capture -> [CDC] -> FIFO -> source (to the USB3 Bulk IN)
                (sample)            (sys)

- Control (quasi-static configuration, enable) resynchronized to the sample clock domain, status and
  counters resynchronized to sys (counters: stable once the capture is done).
- start: rising edge of the capture enable in sys (to flush the stale data of the USB3 Bulk IN).
- Trigger/pre-trigger window: the FIFO is the pre-trigger buffer. While armed, the FIFO output is not
  forwarded and its oldest words are dropped (FIFO level kept under pretrigger + PRETRIGGER_MARGIN words).
  Once the trigger status reaches sys, the words before word W - pretrigger are dropped (W: word of the
  trigger sample, packing contiguous from the capture start) and the stream is forwarded: the stream
  starts at word max(0, W - pretrigger), the host computes the trigger offset from trigger_count.
- Outputs (gateware/output.py, outputs > 0): tristate outputs (AUX IOs, output channels) driven from the
  capture events in the cd clock domain (out_o/out_oe to the pads, out_i: pad levels readback).
- Sample clock counter (Gray coded, 32-bit): sample clock frequency measured by the host against its own
  time base (the sample clock accuracy is the samplerate accuracy).
"""

from migen import *
from migen.genlib.cdc import MultiReg, BusSynchronizer, GrayCounter

from litex.gen import *

from litex.soc.interconnect.csr import *
from litex.soc.interconnect     import stream

from gateware.capture   import Capture
from gateware.generator import PatternGenerator
from gateware.output    import Output

# Constants ----------------------------------------------------------------------------------------

ANALYZER_MAGIC   = 0x534c4f47 # "SLOG".
ANALYZER_VERSION = 0x00000003 # Major[31:16], Minor[15:0] (0.2: hardware trigger, 0.3: outputs).

PRETRIGGER_MARGIN = 64 # FIFO words kept above the pre-trigger window while armed (CDC/status latency).

# Analyzer -----------------------------------------------------------------------------------------

class Analyzer(LiteXModule):
    def __init__(self, nchannels=16, ratio=8, data_width=64, base_rate=800e6, fifo_depth=2048,
        cd="sample", naux=0, output_channels=[]):
        self.samples = Signal(ratio*nchannels)                     # i: Samples (cd clock domain).
        self.source  = source = stream.Endpoint([("data", data_width)]) # o: Data (sys clock domain).
        self.start   = Signal()                                    # o: Capture start (sys, pulse).

        self.nchannels  = nchannels
        self.ratio      = ratio
        self.base_rate  = base_rate
        self.fifo_depth = fifo_depth

        # # #

        # Capture ----------------------------------------------------------------------------------
        # data_width capture words (converted in the cd clock domain): the CDC to sys carries the
        # full rate (32-bit words: 4 bytes per sys cycle, 250MB/s at 62.5MHz, too low for 400MB/s).
        capture = Capture(nchannels, ratio, data_width=data_width)
        self.capture = capture = ClockDomainsRenamer(cd)(capture)
        self.comb += capture.samples.eq(self.samples)

        # Pattern Generator (restarts with each capture).
        self.generator = generator = ClockDomainsRenamer(cd)(PatternGenerator(nchannels, ratio))
        self.comb += [
            generator.enable.eq(capture.enable),
            capture.generator.eq(generator.word),
        ]

        # Control (sys -> cd).
        self.enable  = Signal()
        self.mode    = Signal(2)
        self.divider = Signal(32, reset=1)
        self.pattern = Signal(2)
        self.limit   = Signal(48)
        self.trigger_enable  = Signal()
        self.trigger_zero    = Signal(nchannels)
        self.trigger_one     = Signal(nchannels)
        self.trigger_rising  = Signal(nchannels)
        self.trigger_falling = Signal(nchannels)
        self.pretrigger      = Signal(16) # Pre-trigger window (64-bit words).
        controls = [
            (self.enable,          capture.enable),
            (self.mode,            capture.mode),
            (self.divider,         capture.divider),
            (self.pattern,         capture.pattern),
            (self.limit,           capture.limit),
            (self.trigger_enable,  capture.trigger_enable),
            (self.trigger_zero,    capture.trigger_zero),
            (self.trigger_one,     capture.trigger_one),
            (self.trigger_rising,  capture.trigger_rising),
            (self.trigger_falling, capture.trigger_falling),
        ]
        for i, o in controls:
            if cd == "sys":
                self.comb += o.eq(i)
            else:
                self.specials += MultiReg(i, o, cd)
        enable_d = Signal()
        self.sync += enable_d.eq(self.enable)
        self.comb += self.start.eq(self.enable & ~enable_d)

        # Status (cd -> sys).
        self.running  = Signal()
        self.done     = Signal()
        self.overflow = Signal()
        self.error    = Signal()
        self.count    = Signal(64)
        self.triggered     = Signal()
        self.trigger_count = Signal(64)
        status = Cat(capture.running, capture.done, capture.overflow, capture.error)
        if cd == "sys":
            self.comb += [
                Cat(self.running, self.done, self.overflow, self.error).eq(status),
                self.count.eq(capture.count),
            ]
        else:
            self.specials += MultiReg(status, Cat(self.running, self.done, self.overflow, self.error))
            self.count_sync = count_sync = BusSynchronizer(len(capture.count), cd, "sys")
            self.comb += [
                count_sync.i.eq(capture.count),
                self.count.eq(count_sync.o),
            ]

        # Trigger status (cd -> sys): consistent snapshot of trigger_count/triggered/gen, accepted for the
        # current capture only (gen: toggled on each start in both domains).
        trig_status = Cat(capture.trigger_count, capture.triggered, capture.gen)
        trig_sys    = Signal(len(trig_status))
        if cd == "sys":
            self.comb += trig_sys.eq(trig_status)
        else:
            self.trig_sync = trig_sync = BusSynchronizer(len(trig_status), cd, "sys")
            self.comb += [
                trig_sync.i.eq(trig_status),
                trig_sys.eq(trig_sync.o),
            ]
        cbits = len(capture.trigger_count)
        gen   = Signal()
        self.sync += If(self.start, gen.eq(~gen))
        self.comb += [
            self.triggered.eq(trig_sys[cbits] & (trig_sys[cbits + 1] == gen)),
            self.trigger_count.eq(trig_sys[:cbits]),
        ]

        # Data Path --------------------------------------------------------------------------------
        layout = [("data", data_width)]
        if cd == "sys":
            cdc_source = capture.source
        else:
            self.cdc = cdc = stream.ClockDomainCrossing(layout, cd_from=cd, cd_to="sys", depth=16)
            self.comb += capture.source.connect(cdc.sink)
            cdc_source = cdc.source
        # FIFO reset on start: stale data of an aborted capture dropped.
        self.fifo = fifo = ResetInserter()(stream.SyncFIFO(layout, fifo_depth, buffered=True))
        self.comb += [
            fifo.reset.eq(self.start),
            cdc_source.connect(fifo.sink),
        ]

        # Pre-trigger window -----------------------------------------------------------------------
        # head: FIFO words read (forwarded or dropped) since the start, target: first word of the
        # window (registered from the trigger status, valid with trig_ok, 2 cycles after triggered).
        head      = Signal(cbits)
        trig_word = Signal(cbits)
        target    = Signal(cbits)
        trig_d    = Signal(2)
        trig_ok   = Signal()
        forward   = Signal()
        drop      = Signal()
        threshold = Signal(17)
        spw_log2  = {0: 4, 1: 3, 2: 2} # log2(samples per 64-bit word) per channel mode.
        assert data_width == 64
        self.sync += [
            Case(self.mode, {
                0         : trig_word.eq(self.trigger_count[spw_log2[0]:]),
                1         : trig_word.eq(self.trigger_count[spw_log2[1]:]),
                "default" : trig_word.eq(self.trigger_count[spw_log2[2]:]),
            }),
            If(trig_word >= self.pretrigger,
                target.eq(trig_word - self.pretrigger)
            ).Else(
                target.eq(0)
            ),
            trig_d.eq(Cat(self.triggered & ~self.start, trig_d[0] & self.triggered & ~self.start)),
            threshold.eq(self.pretrigger + PRETRIGGER_MARGIN),
        ]
        self.comb += [
            trig_ok.eq(trig_d[1]),
            If(trig_ok,
                drop.eq(head < target)
            ).Else(
                drop.eq(fifo.level > threshold)
            ),
            If(forward,
                fifo.source.connect(source)
            ).Else(
                fifo.source.ready.eq(drop)
            ),
        ]
        self.sync += [
            If(self.start,
                head.eq(0),
                forward.eq(~self.trigger_enable),
            ).Else(
                If(fifo.source.valid & fifo.source.ready,
                    head.eq(head + 1)
                ),
                If(trig_ok & ~drop,
                    forward.eq(1)
                )
            )
        ]

        # Outputs ----------------------------------------------------------------------------------
        # Outputs 0..naux-1: AUX IOs, then the output channels (ascending). Controls resynchronized to
        # cd (quasi-static, gpio/oe: a few cycles of latency from the host), pad levels to sys.
        self.naux            = naux
        self.output_channels = sorted(output_channels)
        self.noutputs        = n = naux + len(self.output_channels)
        self.out_o      = Signal(max(n, 1)) # o: Output levels (cd).
        self.out_oe     = Signal(max(n, 1)) # o: Output enables (cd).
        self.out_i      = Signal(max(n, 1)) # i: Pad levels (cd, readback).
        self.out_ctrl   = Signal(24)        # oe[0:8], invert[8:16], gpio[16:24] (sys).
        self.out_sel    = Signal(32)
        self.out_pulse  = Signal(32)        # delay[0:24], width[24:32] (sys).
        self.out_high   = Signal(24)
        self.out_low    = Signal(24)
        self.out_levels = Signal(8)         # Pad levels (sys).
        if n:
            self.output = output = ClockDomainsRenamer(cd)(Output(n))
            out_start = Signal()
            enable_cd = Signal()
            sync = getattr(self.sync, cd)
            sync += enable_cd.eq(capture.enable)
            self.comb += [
                out_start.eq(capture.enable & ~enable_cd),
                output.start.eq(out_start),
                output.trigger.eq(capture.trigger_pulse),
                output.armed.eq(capture.armed),
                output.running.eq(capture.running),
                output.generator.eq(generator.word[0:8]),
                self.out_o.eq(output.o),
                self.out_oe.eq(output.oe_o),
            ]
            out_controls = [
                (self.out_ctrl[0:n],       output.oe),
                (self.out_ctrl[8:8 + n],   output.invert),
                (self.out_ctrl[16:16 + n], output.gpio),
                (self.out_sel[:4*n],       output.sel),
                (self.out_pulse[24:32],    output.width),
                (self.out_pulse[0:24],     output.delay),
                (self.out_high,            output.high),
                (self.out_low,             output.low),
            ]
            for i, o in out_controls:
                if cd == "sys":
                    self.comb += o.eq(i)
                else:
                    self.specials += MultiReg(i, o, cd)
            if cd == "sys":
                self.comb += self.out_levels[:n].eq(self.out_i)
            else:
                self.specials += MultiReg(self.out_i, self.out_levels[:n])

        # Sample Clock Counter ---------------------------------------------------------------------
        self.clk_count = Signal(32)
        if cd == "sys":
            self.sync += self.clk_count.eq(self.clk_count + 1)
        else:
            gray = ClockDomainsRenamer(cd)(GrayCounter(32))
            self.submodules += gray
            self.comb += gray.ce.eq(1)
            gray_sys = Signal(32)
            self.specials += MultiReg(gray.q, gray_sys)
            # Gray to binary with per bit signals (no combinatorial self-reference).
            binary = [Signal() for _ in range(32)]
            self.comb += binary[31].eq(gray_sys[31])
            self.comb += [binary[i].eq(binary[i + 1] ^ gray_sys[i]) for i in range(31)]
            self.sync += self.clk_count.eq(Cat(*binary))

    def add_csr(self):
        # Info.
        self._magic     = CSRStatus(32, reset=ANALYZER_MAGIC,   description="Magic (\"SLOG\").")
        self._version   = CSRStatus(32, reset=ANALYZER_VERSION, description="Gateware version.")
        self._config    = CSRStatus(fields=[
            CSRField("nchannels",  size=8,  offset=0,  reset=self.nchannels),
            CSRField("ratio",      size=8,  offset=8,  reset=self.ratio),
            CSRField("fifo_depth", size=16, offset=16, reset=self.fifo_depth//64 if self.fifo_depth >= 64 else 1,
                description="FIFO depth (in 64-word units)."),
        ])
        self._base_rate = CSRStatus(32, reset=int(self.base_rate), description="Base samplerate (Hz).")

        # Control.
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
                ("``0b10``", "Pattern generator (UART, SPI, PWM, clock, Gray counter)."),
            ]),
        ])
        self._divider = CSRStorage(32, reset=1, description="Decimation (samplerate = base_rate/divider).")
        self._limit   = CSRStorage(64, description="Samples to capture (0: until disabled).")

        # Status.
        self._status  = CSRStatus(fields=[
            CSRField("running",  size=1, offset=0),
            CSRField("done",     size=1, offset=1),
            CSRField("overflow", size=1, offset=2, description="Data lost (host too slow): capture stopped."),
            CSRField("error",    size=1, offset=3, description="Invalid configuration (> 32 bits/cycle)."),
            CSRField("triggered", size=1, offset=4, description="Hardware trigger matched (trigger_count valid)."),
        ])
        self._count     = CSRStatus(64, description="Captured samples.")
        self._clk_count = CSRStatus(32, description="Sample clock cycles (free running).")

        # Trigger (appended: the registers above keep their addresses).
        self._trigger = CSRStorage(fields=[
            CSRField("enable", size=1, offset=0, description="Hardware trigger enable (capture armed until the trigger)."),
        ])
        self._trigger_level = CSRStorage(fields=[
            CSRField("zero", size=16, offset=0,  description="Channels low."),
            CSRField("one",  size=16, offset=16, description="Channels high."),
        ])
        self._trigger_edge = CSRStorage(fields=[
            CSRField("rising",  size=16, offset=0,  description="Channels rising (rising + falling: any edge)."),
            CSRField("falling", size=16, offset=16, description="Channels falling."),
        ])
        self._pretrigger    = CSRStorage(16, description="Pre-trigger window (64-bit words, < FIFO depth - 2*margin).")
        self._trigger_count = CSRStatus(64,  description="Trigger sample index in the capture (valid when triggered).")

        # Outputs (appended: the registers above keep their addresses).
        channels_mask = sum(1 << c for c in self.output_channels)
        self._out_config = CSRStatus(fields=[
            CSRField("channels", size=16, offset=0,  reset=channels_mask, description="Output channels (mask)."),
            CSRField("naux",     size=4,  offset=16, reset=self.naux,     description="AUX IOs (outputs 0..naux-1)."),
        ])
        self._out_ctrl = CSRStorage(fields=[
            CSRField("oe",     size=8, offset=0,  description="Output enables (0: tristate)."),
            CSRField("invert", size=8, offset=8,  description="Output inverts."),
            CSRField("gpio",   size=8, offset=16, description="GPIO levels (source 0)."),
        ])
        self._out_sel    = CSRStorage(32, description="Output sources (4 bits per output, gateware/output.py).")
        # Timings (sample clock cycles): write-only (no read multiplexer: logic).
        self._out_pulse  = CSR(32) # Trigger pulse delay[0:24], width[24:32].
        self._out_high   = CSR(24) # Periodic high time.
        self._out_low    = CSR(24) # Periodic low time.
        self._out_levels = CSRStatus(8, description="Pad levels (readback).")
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
            self._clk_count.status.eq(self.clk_count),
            self.trigger_enable.eq(self._trigger.fields.enable),
            self.trigger_zero.eq(self._trigger_level.fields.zero),
            self.trigger_one.eq(self._trigger_level.fields.one),
            self.trigger_rising.eq(self._trigger_edge.fields.rising),
            self.trigger_falling.eq(self._trigger_edge.fields.falling),
            self.pretrigger.eq(self._pretrigger.storage),
            self._status.fields.triggered.eq(self.triggered),
            self._trigger_count.status.eq(self.trigger_count),
            self.out_ctrl.eq(self._out_ctrl.storage),
            self.out_sel.eq(self._out_sel.storage),
            self._out_levels.status.eq(self.out_levels),
        ]
        self.sync += [
            If(self._out_pulse.re, self.out_pulse.eq(self._out_pulse.r)),
            If(self._out_high.re,  self.out_high.eq(self._out_high.r)),
            If(self._out_low.re,   self.out_low.eq(self._out_low.r)),
        ]
