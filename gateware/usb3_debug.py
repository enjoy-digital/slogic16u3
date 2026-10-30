#
# This file is part of SLogic16U3.
#
# Copyright (c) 2026 Florent Kermarrec <florent@enjoy-digital.fr>
# SPDX-License-Identifier: BSD-2-Clause

"""USB3 bring-up debug (CSRs, read over JTAGBone when the USB3 link is not up): GTR12 lane status
transitions, PIPE counters, RX/TX snapshots, LTSSM history (as the USB3-PIPE Tang Mega 138K Pro bench
target), read by software/debug/usb3_debug.py."""

from migen import *
from migen.genlib.cdc import MultiReg, PulseSynchronizer

from litex.gen import *

from litex.soc.interconnect.csr import *

from usb3_pipe.common import COM, K

# USB3 Debug ---------------------------------------------------------------------------------------

class USB3Debug(LiteXModule):
    """USB3 bring-up debug.

    gtr12: LiteICLink GTR12, serdes: GW5USB3SerDes (renamed to cd), phy: USB3PHY (LTSSM in sys, PIPE in
    cd), cd: PIPE/SerDes clock domain (GTR12 TX clock). Wide values are read 32-bit word by word:
    <name>_sel (word index), <name>_data (word). with_tx_snapshots: SerDes RX words snapshot and TX
    snapshot/ring (~3K LUTs, link in U0: do not fit with the USB3 Device Core on the GW5AT-15).
    with_lane_counters: GTR12 lane status transitions and PIPE counters (link training),
    with_rx_snapshots: GTR12 raw RX words from a K28.5 (rx clock domain: cleared on RX resets). core: USB3
    Device Core, Data Packets rings (Link Layer RX DPP, EP0 RX).
    """
    def __init__(self, gtr12, serdes, phy, cd, sys_clk_freq, core=None,
        with_lane_counters = False,
        with_rx_snapshots  = False,
        with_tx_snapshots  = False):
        self.cd = cd

        # # #

        if with_lane_counters:
            self.add_gtr12_transitions(gtr12)
            self.add_pipe_counters(phy.pipe, serdes)
        if with_rx_snapshots or with_tx_snapshots:
            self.add_rx_snapshots(gtr12, serdes, phy.pipe if with_tx_snapshots else None)
        self.add_ltssm_history(phy, sys_clk_freq, n=16)
        if core is not None:
            self.add_dpp_rings(core)

    def add_ring(self, name, value, we, n=16):
        """Last n values (written on we, newest first), frozen while ring_freeze is set."""
        ring = [Signal(len(value)) for _ in range(n)]
        self.sync += If(we & ~self._ring_freeze.storage,
            ring[0].eq(value),
            *[ring[i].eq(ring[i-1]) for i in range(1, n)]
        )
        self.add_indexed_status(name, Cat(*[Cat(r, Constant(0, -len(r) % 32)) for r in ring]))

    def add_dpp_rings(self, core):
        """Data Packets rings (EP0 Setup/Data stages): Link Layer RX DPP words (data[31:0], ept, first,
        last, setup, valid, done, ok) and EP0 RX words (data, first, last, setup)."""
        self._ring_freeze = CSRStorage()
        dpp  = core.link.rx_dpp
        done = core.link.rx_dpp_done
        ok   = core.link.rx_dpp_ok
        # 16x (data[32], ept[4], first, last, setup, valid&ready, done, ok) (64-bit entries).
        self.add_ring("link_rx_dpp", Cat(dpp.data[:32], dpp.ept, dpp.first, dpp.last, dpp.setup,
            dpp.valid & dpp.ready, done, ok), (dpp.valid & dpp.ready) | done)
        ep0 = core.ep0.rx_dpp
        # 16x (data[32], first, last, setup) (64-bit entries).
        self.add_ring("ep0_rx", Cat(ep0.data, ep0.first, ep0.last, core.ep0.rx_dpp_setup),
            ep0.valid & ep0.ready)

    def add_indexed_status(self, name, value):
        words = (len(value) + 31)//32
        sel   = CSRStorage(bits_for(max(words - 1, 1)), name=f"{name}_sel")
        data  = CSRStatus(32, name=f"{name}_data")
        setattr(self, f"_{name}_sel",  sel)
        setattr(self, f"_{name}_data", data)
        cases = {i: data.status.eq(value[32*i:min(32*(i+1), len(value))]) for i in range(words)}
        self.sync += Case(sel.storage, cases)

    def add_gtr12_transitions(self, gtr12):
        """Transitions counters of the GTR12 lane status bits (rx_elecidle, astat[0:6], stat[0:13]),
        sampled in the reset-less RX clock domain: LFPS/Electrical Idle indicators identification."""
        self.cd_rx_nr = ClockDomain(reset_less=True)
        self.comb += self.cd_rx_nr.clk.eq(ClockSignal("rx"))
        bits   = [gtr12.rx_elecidle] + [gtr12.astat[i] for i in range(6)] + [gtr12.stat[i] for i in range(13)]
        counts = []
        for b in bits:
            b_r = Signal()
            b_d = Signal()
            c   = Signal(16)
            self.sync.rx_nr += [
                b_r.eq(b),
                b_d.eq(b_r),
                If(b_r != b_d, c.eq(c + 1)),
            ]
            cs = Signal(16)
            self.specials += MultiReg(c, cs)
            counts.append(cs)
        # Transitions counts: rx_elecidle, astat[0:6], stat[0:13] (16-bit each, wrapping).
        self.add_indexed_status("gtr12_transitions", Cat(*counts))

    def add_pipe_counters(self, pipe, serdes):
        """PIPE counters (16-bit wrapping, resynchronized): SerDes RX words with a COM / K28.4 (decode
        error substitution) symbol, TS1/TS1 inverted/TS2 detections."""
        src = serdes.source
        def has(symbol):
            return src.valid & src.ready & Reduce("OR", [src.ctrl[i] & (src.data[8*i:8*(i+1)] == symbol)
                for i in range(4)])
        events = [has(COM.value), has(K(28, 4)), pipe.ts.rx_ts1, pipe.ts.rx_ts1_inv, pipe.ts.rx_ts2]
        counts = []
        sync   = getattr(self.sync, self.cd)
        for e in events:
            e_r = Signal()
            c   = Signal(16)
            sync += [e_r.eq(e), If(e_r, c.eq(c + 1))]
            cs = Signal(16)
            self.specials += MultiReg(c, cs)
            counts.append(cs)
        # COM words, K28.4 words, TS1/TS1 inverted/TS2 detections (16-bit each).
        self.add_indexed_status("pipe_counters", Cat(*counts))

    def add_ltssm_history(self, phy, sys_clk_freq, n=32):
        """LTSSM states history: last n states (8-bit) with their entry time (24-bit, us), frozen while
        ltssm_history_freeze is set."""
        self._ltssm_history_freeze = CSRStorage()
        us      = Signal(24)
        tick    = Signal(max=int(sys_clk_freq/1e6))
        state_d = Signal(8)
        hist    = [Signal(32) for _ in range(n)]
        self.sync += [
            tick.eq(tick + 1),
            If(tick == int(sys_clk_freq/1e6) - 1, tick.eq(0), us.eq(us + 1)),
            state_d.eq(phy.ltssm_state),
            If((phy.ltssm_state != state_d) & ~self._ltssm_history_freeze.storage,
                hist[0].eq(Cat(phy.ltssm_state, us)),
                *[hist[i].eq(hist[i-1]) for i in range(1, n)]
            ),
        ]
        # n x (state[8], time_us[24]), newest first.
        self.add_indexed_status("ltssm_history", Cat(*hist))

    def add_rx_snapshots(self, gtr12, serdes, pipe=None):
        """RX/TX snapshots (captured registers static when read), re-armed on each rx_snapshot_arm
        write: 8 cycles of GTR12 raw words and decoders outputs from a K28.5 in the raw words (rx clock
        domain), 16 SerDes RX words, 32 SerDes TX words/encoders outputs from the end of the Training
        Sequences transmission, last 64 PIPE TX words in U0 (frozen on the U0 exit) (TX: with pipe)."""
        self._rx_snapshot_arm = CSR()
        def snapshot(domain, n, data, trigger):
            arm = PulseSynchronizer("sys", domain)
            self.submodules += arm
            self.comb += arm.i.eq(self._rx_snapshot_arm.re)
            run   = Signal()
            count = Signal(max=n + 1)
            regs  = [Signal(len(data)) for _ in range(n)]
            sync  = getattr(self.sync, domain)
            # Registered data/trigger (timings).
            data_r    = Signal(len(data))
            trigger_r = Signal()
            sync += [data_r.eq(data), trigger_r.eq(trigger)]
            data, trigger = data_r, trigger_r
            sync += [
                If(arm.o,
                    run.eq(0), count.eq(0),
                ).Elif(~run & (count == 0) & trigger,
                    run.eq(1),
                ),
                If((run | ((count == 0) & trigger)) & (count != n),
                    Case(count, {i: regs[i].eq(data) for i in range(n)}),
                    count.eq(count + 1),
                ),
            ]
            return Cat(*regs)
        raw  = gtr12.rx_raw
        k285 = Reduce("OR", [(raw[10*i:10*(i+1)] == 0x17c) | (raw[10*i:10*(i+1)] == 0x283) for i in range(4)])
        dec  = Cat(raw, *[Cat(d.d, d.k, d.invalid) for d in gtr12.decoders])
        src  = serdes.source
        # 8x (raw[40], 4x (d[8], k, invalid)) / 16x (data[32], ctrl[4], valid).
        self.add_indexed_status("rx_snapshot_raw",   snapshot("rx", 8, dec, k285))
        if pipe is None:
            return
        self.add_indexed_status("rx_snapshot_words", snapshot(self.cd, 16, Cat(src.data, src.ctrl, src.valid), 1))

        # TX snapshot: from the end of the Training Sequences transmission.
        sync        = getattr(self.sync, self.cd)
        tx_active_d = Signal()
        sync += tx_active_d.eq(pipe.ts.tx_active)
        ts_end = tx_active_d & ~pipe.ts.tx_active
        snk    = serdes.sink
        # 32x (data[32], ctrl[4], accepted, 4x encoder output[10]).
        self.add_indexed_status("tx_snapshot", snapshot(self.cd, 32, Cat(snk.data, snk.ctrl,
            snk.valid & snk.ready, *gtr12.encoder.output), ts_end))

        # TX ring: last 64 PIPE TX words in U0, frozen on the PIPE U0 exit (tx_ready falling).
        arm = PulseSynchronizer("sys", self.cd)
        self.submodules += arm
        self.comb += arm.i.eq(self._rx_snapshot_arm.re)
        psnk    = pipe.sink
        ring    = [Signal(36) for _ in range(64)]
        frozen  = Signal()
        ready_d = Signal()
        sync += [
            ready_d.eq(pipe.tx_ready),
            If(arm.o, frozen.eq(0)).Elif(ready_d & ~pipe.tx_ready, frozen.eq(1)),
            If(~frozen & psnk.valid & psnk.ready,
                ring[0].eq(Cat(psnk.data, psnk.ctrl)),
                *[ring[i].eq(ring[i-1]) for i in range(1, 64)]
            ),
        ]
        # 64x (data[32], ctrl[4]), newest first.
        self.add_indexed_status("tx_ring", Cat(*ring))
