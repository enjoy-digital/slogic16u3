#
# This file is part of SLogic16U3.
#
# Copyright (c) 2026 Florent Kermarrec <florent@enjoy-digital.fr>
# SPDX-License-Identifier: BSD-2-Clause

"""Flash: SPI Flash master for the configuration SPI Flash (remote update).

- Pins: npads tristate pads (MSPI), CLK/CS_N/MOSI/MISO roles mapped at runtime to any pad (the pads
  functions are probed by the host: JEDEC ID read with each mapping). All pads are inputs while the
  core is disabled (default), pads in `high` are driven high when enabled (ex: WP_N/HOLD_N).
- Transfers: full-duplex SPI mode 0 transfers of `length` bytes from a buffer (Wishbone, bytes in
  little-endian words): bytes sent from the TX buffer (Wishbone writes), bytes received to the RX buffer
  (Wishbone reads, at the same offsets). CS_N asserted during the transfer, kept asserted after it with
  `hold` (multi-part commands). TX/RX buffers: simple dual port memories (BSRAMs).

A 512-byte buffer covers a Page Program (4-byte header + 256 bytes) or a read of up to 508 bytes in a
single transfer.
"""

from migen import *
from migen.genlib.cdc import MultiReg

from litex.gen import *

from litex.soc.interconnect.csr import *
from litex.soc.interconnect     import wishbone

# Constants ----------------------------------------------------------------------------------------

FLASH_ROLES = ["clk", "cs_n", "mosi", "miso"]

# Flash --------------------------------------------------------------------------------------------

class Flash(LiteXModule):
    def __init__(self, pads=None, npads=6, buffer_size=512, default_pins=None, default_enable=False):
        npads = npads if pads is None else len(pads)
        assert npads <= 8
        assert buffer_size in [256, 512, 1024]
        self.bus = bus = wishbone.Interface(data_width=32, address_width=32, addressing="word")
        self.buffer_size = buffer_size

        # Control.
        self.enable  = Signal(reset=default_enable)  # i: Pads driven (else all inputs).
        self.pins    = [Signal(3, name=f"pin_{r}") for r in FLASH_ROLES] # i: Pad index of each role.
        self.high    = Signal(npads)                 # i: Pads driven high when enabled.
        self.divider = Signal(8, reset=2)            # i: SPI clock = sys/(2*(divider + 1)), divider >= 2.
        self.length  = Signal(max=buffer_size + 1)   # i: Transfer length (bytes).
        self.hold    = Signal()                      # i: Keep CS_N asserted after the transfer.
        self.start   = Signal()                      # i: Start a transfer (pulse).
        self.release = Signal()                      # i: Release CS_N (pulse, after a hold).

        # Status.
        self.busy = Signal() # o

        # Pads (without pads: tristate signals for simulation).
        self.pads_o  = Signal(npads)
        self.pads_oe = Signal(npads)
        self.pads_i  = Signal(npads)

        if default_pins is not None:
            for sig, pad in zip(self.pins, default_pins):
                sig.reset = pad

        # # #

        # Buffer -----------------------------------------------------------------------------------
        words  = buffer_size//4
        tx_mem = Memory(32, words)
        rx_mem = Memory(32, words)
        tx_wr  = tx_mem.get_port(write_capable=True, we_granularity=8)
        tx_rd  = tx_mem.get_port(has_re=False)
        rx_wr  = rx_mem.get_port(write_capable=True, we_granularity=8)
        rx_rd  = rx_mem.get_port(has_re=False)
        self.specials += tx_mem, tx_wr, tx_rd, rx_mem, rx_wr, rx_rd

        # Wishbone: writes to the TX buffer, reads from the RX buffer (ack one cycle after the
        # request: synchronous read).
        self.comb += [
            tx_wr.adr.eq(bus.adr[:log2_int(words)]),
            tx_wr.dat_w.eq(bus.dat_w),
            [tx_wr.we[i].eq(bus.cyc & bus.stb & bus.we & bus.sel[i] & ~bus.ack) for i in range(4)],
            rx_rd.adr.eq(bus.adr[:log2_int(words)]),
            bus.dat_r.eq(rx_rd.dat_r),
        ]
        self.sync += [
            bus.ack.eq(0),
            If(bus.cyc & bus.stb & ~bus.ack, bus.ack.eq(1)),
        ]

        # Pads -------------------------------------------------------------------------------------
        clk  = Signal()
        cs_n = Signal(reset=1)
        mosi = Signal()
        miso = Signal()
        pins_i = Signal(npads)
        clk_sel, cs_sel, mosi_sel, miso_sel = self.pins
        for p in range(npads):
            is_clk  = (clk_sel  == p)
            is_cs   = (cs_sel   == p)
            is_mosi = (mosi_sel == p)
            self.comb += [
                self.pads_oe[p].eq(self.enable & (is_clk | is_cs | is_mosi | self.high[p])),
                If(is_clk,
                    self.pads_o[p].eq(clk)
                ).Elif(is_cs,
                    self.pads_o[p].eq(cs_n)
                ).Elif(is_mosi,
                    self.pads_o[p].eq(mosi)
                ).Else(
                    self.pads_o[p].eq(1)
                ),
            ]
            if pads is not None:
                t = TSTriple()
                self.specials += t.get_tristate(pads[p])
                self.comb += [
                    t.oe.eq(self.pads_oe[p]),
                    t.o.eq(self.pads_o[p]),
                    self.pads_i[p].eq(t.i),
                ]
        self.specials += MultiReg(self.pads_i, pins_i)
        self.comb += miso.eq(Array(pins_i)[miso_sel])

        # SPI Engine -------------------------------------------------------------------------------
        # Half period >= 3 cycles: MISO (resynchronized, 2 cycles latency) sampled at the end of the
        # CLK high phase.
        tick    = Signal()
        tickc   = Signal(8)
        divider = Signal(8)
        self.comb += divider.eq(Mux(self.divider < 2, 2, self.divider))
        self.sync += [
            tick.eq(0),
            If(tickc == 0,
                tickc.eq(divider),
                tick.eq(1),
            ).Else(
                tickc.eq(tickc - 1)
            )
        ]
        index = Signal(max=buffer_size + 1) # Byte index.
        bit   = Signal(3)
        shift = Signal(8)
        held  = Signal()
        byte_sel = Signal(2)
        self.comb += [
            tx_rd.adr.eq(index[2:]),
            rx_wr.adr.eq(index[2:]),
            byte_sel.eq(index[:2]),
            rx_wr.dat_w.eq(Replicate(shift, 4)),
        ]

        self.fsm = fsm = FSM(reset_state="IDLE")
        fsm.act("IDLE",
            If(self.release, NextValue(held, 0)),
            If(self.start & (self.length != 0),
                NextValue(index, 0),
                NextState("LOAD")
            )
        )
        self.comb += cs_n.eq(~(held | ~fsm.ongoing("IDLE")))
        # Load the byte (synchronous read: one cycle).
        fsm.act("LOAD",
            self.busy.eq(1),
            NextState("LOAD-WAIT")
        )
        fsm.act("LOAD-WAIT",
            self.busy.eq(1),
            NextValue(shift, Array([tx_rd.dat_r[8*i:8*(i + 1)] for i in range(4)])[byte_sel]),
            NextValue(bit, 7),
            If(tick, NextState("LOW"))
        )
        # Mode 0: MOSI set with CLK low, MISO sampled on the CLK rising edge.
        fsm.act("LOW",
            self.busy.eq(1),
            mosi.eq(shift[7]),
            If(tick, NextState("HIGH"))
        )
        fsm.act("HIGH",
            self.busy.eq(1),
            clk.eq(1),
            mosi.eq(shift[7]),
            If(tick,
                NextValue(shift, Cat(miso, shift[:7])),
                If(bit == 0,
                    NextState("STORE")
                ).Else(
                    NextValue(bit, bit - 1),
                    NextState("LOW")
                )
            )
        )
        fsm.act("STORE",
            self.busy.eq(1),
            [rx_wr.we[i].eq(byte_sel == i) for i in range(4)],
            NextValue(index, index + 1),
            If(index + 1 == self.length,
                NextValue(held, self.hold),
                NextState("END")
            ).Else(
                NextState("LOAD")
            )
        )
        # CS_N high time (or kept asserted with hold).
        fsm.act("END",
            self.busy.eq(1),
            If(tick, NextState("IDLE"))
        )

    def add_csr(self):
        self._control = CSRStorage(fields=[
            CSRField("enable",  size=1, offset=0, reset=self.enable.reset.value, description="Drive the Flash pads (else all inputs)."),
            CSRField("clk",     size=3, offset=4,  reset=self.pins[0].reset.value, description="CLK pad."),
            CSRField("cs_n",    size=3, offset=8,  reset=self.pins[1].reset.value, description="CS_N pad."),
            CSRField("mosi",    size=3, offset=12, reset=self.pins[2].reset.value, description="MOSI pad."),
            CSRField("miso",    size=3, offset=16, reset=self.pins[3].reset.value, description="MISO pad."),
            CSRField("high",    size=8, offset=20, description="Pads driven high (ex: WP_N/HOLD_N)."),
        ])
        self._divider = CSRStorage(8, reset=2, description="SPI clock = sys/(2*(divider + 1)), divider >= 2.")
        self._xfer    = CSRStorage(fields=[
            CSRField("length",  size=16, offset=0, description="Transfer length (bytes)."),
            CSRField("hold",    size=1,  offset=16, description="Keep CS_N asserted after the transfer."),
        ])
        self._ctrl = CSRStorage(fields=[
            CSRField("start",   size=1, offset=0, pulse=True, description="Start the transfer."),
            CSRField("release", size=1, offset=1, pulse=True, description="Release CS_N."),
        ])
        self._status = CSRStatus(fields=[
            CSRField("busy", size=1, offset=0),
        ])
        f = self._control.fields
        self.comb += [
            self.enable.eq(f.enable),
            self.pins[0].eq(f.clk),
            self.pins[1].eq(f.cs_n),
            self.pins[2].eq(f.mosi),
            self.pins[3].eq(f.miso),
            self.high.eq(f.high),
            self.divider.eq(self._divider.storage),
            self.length.eq(self._xfer.fields.length),
            self.hold.eq(self._xfer.fields.hold),
            self.start.eq(self._ctrl.fields.start),
            self.release.eq(self._ctrl.fields.release),
            self._status.fields.busy.eq(self.busy),
        ]
