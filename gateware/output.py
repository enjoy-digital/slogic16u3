#
# This file is part of SLogic16U3.
#
# Copyright (c) 2026 Florent Kermarrec <florent@enjoy-digital.fr>
# SPDX-License-Identifier: BSD-2-Clause

"""Output: tristate outputs driven from the capture events (sample clock domain, fixed latencies).

Each output has an output enable (oe: 0 by default, the pad is tristate until enabled by the host), an
invert bit and a source (sel, 4 bits):

- OUTPUT_GPIO      (0): gpio level (host).
- OUTPUT_TRIGGER   (1): pulse of `width` cycles, `delay` cycles after the hardware trigger (trigger out,
                        delayed events).
- OUTPUT_ARMED     (2): high while waiting for the trigger.
- OUTPUT_CAPTURE   (3): high while capturing (start/stop marker).
- OUTPUT_PERIODIC  (4): high `high` cycles, then low `low` cycles, restarted (high) on each capture start
                        (reference ticks, square waves).
- OUTPUT_GENERATOR (8 + k): pattern generator channel k (k < 8, sample 0 of each cycle: loopback
                        self-test of the inputs).

Timings are in sample clock cycles (100MHz: 10ns): the outputs are registered, latencies fixed (see
doc/GATEWARE.md).
"""

from migen import *

from litex.gen import *

# Constants ----------------------------------------------------------------------------------------

OUTPUT_GPIO      = 0
OUTPUT_TRIGGER   = 1
OUTPUT_ARMED     = 2
OUTPUT_CAPTURE   = 3
OUTPUT_PERIODIC  = 4
OUTPUT_GENERATOR = 8 # + generator channel (0-7).

OUTPUT_MAX = 8 # Outputs (out_sel: 4 bits per output on a 32-bit register).

# Output -------------------------------------------------------------------------------------------

class Output(LiteXModule):
    def __init__(self, n):
        assert 1 <= n <= OUTPUT_MAX
        self.n = n

        # Events.
        self.start     = Signal()  # i: Capture start (pulse).
        self.trigger   = Signal()  # i: Trigger matched (pulse).
        self.armed     = Signal()  # i: Waiting for the trigger.
        self.running   = Signal()  # i: Capturing.
        self.generator = Signal(8) # i: Pattern generator CH0-CH7 (sample 0 of the cycle).

        # Control (quasi-static).
        self.oe     = Signal(n)   # i: Output enable (0: tristate).
        self.gpio   = Signal(n)   # i
        self.invert = Signal(n)   # i
        self.sel    = Signal(4*n) # i: Sources (4 bits per output).
        self.width  = Signal(8)   # i: Trigger pulse width (cycles, 0: no pulse).
        self.delay  = Signal(24)  # i: Trigger pulse delay (cycles).
        self.high   = Signal(24)  # i: Periodic high time (cycles, 0: always low).
        self.low    = Signal(24)  # i: Periodic low time (cycles, 0: always high).

        # Pads.
        self.o    = Signal(n) # o: Output levels (registered).
        self.oe_o = Signal(n) # o: Output enables (registered).

        # # #

        # Trigger Pulse ----------------------------------------------------------------------------
        # One down counter: delay, then width (pulse: trigger + 2 + delay cycles, then width cycles).
        pls_phase = Signal(2) # 0: idle, 1: delay, 2: pulse.
        pls_cnt   = Signal(24, reset_less=True)
        pulse     = Signal()
        self.sync += [
            If(self.start,
                pls_phase.eq(0),
            ).Elif(self.trigger,
                pls_phase.eq(1),
                pls_cnt.eq(self.delay),
            ).Elif(pls_phase != 0,
                If(pls_cnt == 0,
                    If(pls_phase == 1,
                        pls_phase.eq(Mux(self.width == 0, 0, 2)),
                        pls_cnt.eq(self.width - 1),
                    ).Else(
                        pls_phase.eq(0),
                    )
                ).Else(
                    pls_cnt.eq(pls_cnt - 1),
                )
            ),
        ]
        self.comb += pulse.eq(pls_phase == 2)

        # Periodic ---------------------------------------------------------------------------------
        # One down counter: high cycles high, then low cycles low (restarted high on start).
        per_high = Signal()
        per_cnt  = Signal(24, reset_less=True)
        high_m1  = Signal(24, reset_less=True)
        low_m1   = Signal(24, reset_less=True)
        high_0   = Signal(reset_less=True)
        low_0    = Signal(reset_less=True)
        periodic = Signal()
        self.sync += [
            high_m1.eq(self.high - 1),
            low_m1.eq(self.low - 1),
            high_0.eq(self.high == 0),
            low_0.eq(self.low == 0),
            If(self.start,
                per_high.eq(1),
                per_cnt.eq(high_m1),
            ).Elif(per_cnt == 0,
                per_high.eq(~per_high),
                per_cnt.eq(Mux(per_high, low_m1, high_m1)),
            ).Else(
                per_cnt.eq(per_cnt - 1),
            ),
            periodic.eq(~high_0 & (low_0 | per_high)),
        ]

        # Outputs ----------------------------------------------------------------------------------
        for i in range(n):
            value = Signal()
            cases = {
                OUTPUT_GPIO     : value.eq(self.gpio[i]),
                OUTPUT_TRIGGER  : value.eq(pulse),
                OUTPUT_ARMED    : value.eq(self.armed),
                OUTPUT_CAPTURE  : value.eq(self.running),
                OUTPUT_PERIODIC : value.eq(periodic),
                "default"       : value.eq(0),
            }
            for k in range(8):
                cases[OUTPUT_GENERATOR + k] = value.eq(self.generator[k])
            self.comb += Case(self.sel[4*i:4*(i + 1)], cases)
            self.sync += [
                self.o[i].eq(value ^ self.invert[i]),
                self.oe_o[i].eq(self.oe[i]),
            ]
