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
- Sample clock counter (Gray coded, 32-bit): sample clock frequency measured by the host against its own
  time base (the sample clock accuracy is the samplerate accuracy).
"""

from migen import *
from migen.genlib.cdc import MultiReg, BusSynchronizer, GrayCounter

from litex.gen import *

from litex.soc.interconnect.csr import *
from litex.soc.interconnect     import stream

from gateware.capture import Capture

# Constants ----------------------------------------------------------------------------------------

ANALYZER_MAGIC   = 0x534c4f47 # "SLOG".
ANALYZER_VERSION = 0x00010000 # Major[31:16], Minor[15:0].

# Analyzer -----------------------------------------------------------------------------------------

class Analyzer(LiteXModule):
    def __init__(self, nchannels=16, ratio=8, data_width=64, base_rate=800e6, fifo_depth=2048,
        cd="sample"):
        self.samples = Signal(ratio*nchannels)                     # i: Samples (cd clock domain).
        self.source  = source = stream.Endpoint([("data", data_width)]) # o: Data (sys clock domain).
        self.start   = Signal()                                    # o: Capture start (sys, pulse).

        self.nchannels  = nchannels
        self.ratio      = ratio
        self.base_rate  = base_rate
        self.fifo_depth = fifo_depth

        # # #

        # Capture ----------------------------------------------------------------------------------
        self.capture = capture = ClockDomainsRenamer(cd)(Capture(nchannels, ratio, data_width))
        self.comb += capture.samples.eq(self.samples)

        # Control (sys -> cd).
        self.enable  = Signal()
        self.mode    = Signal(2)
        self.divider = Signal(32, reset=1)
        self.pattern = Signal(2)
        self.limit   = Signal(48)
        if cd == "sys":
            self.comb += [
                capture.enable.eq(self.enable),
                capture.mode.eq(self.mode),
                capture.divider.eq(self.divider),
                capture.pattern.eq(self.pattern),
                capture.limit.eq(self.limit),
            ]
        else:
            self.specials += [
                MultiReg(self.enable,  capture.enable,  cd),
                MultiReg(self.mode,    capture.mode,    cd),
                MultiReg(self.divider, capture.divider, cd),
                MultiReg(self.pattern, capture.pattern, cd),
                MultiReg(self.limit,   capture.limit,   cd),
            ]
        enable_d = Signal()
        self.sync += enable_d.eq(self.enable)
        self.comb += self.start.eq(self.enable & ~enable_d)

        # Status (cd -> sys).
        self.running  = Signal()
        self.done     = Signal()
        self.overflow = Signal()
        self.error    = Signal()
        self.count    = Signal(64)
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
            fifo.source.connect(source),
        ]

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
        ])
        self._count     = CSRStatus(64, description="Captured samples.")
        self._clk_count = CSRStatus(32, description="Sample clock cycles (free running).")
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
        ]
