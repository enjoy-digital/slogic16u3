#
# This file is part of SLogic16U3.
#
# Copyright (c) 2026 Florent Kermarrec <florent@enjoy-digital.fr>
# SPDX-License-Identifier: BSD-2-Clause

"""Outputs: Output core sources (GPIO, trigger pulse, armed, capture, periodic, generator), invert,
output enables, and the fixed latencies through the Analyzer (Migen sim, single clock domain)."""

import unittest

from migen import *

from gateware.output    import *
from gateware.analyzer  import Analyzer
from gateware.generator import generator_sample

# Latencies (sample clock cycles, documented in doc/GATEWARE.md, used by the simulated device) -----

# Trigger pulse (delay 0): first output cycle - cycle of the trigger sample at the Analyzer samples input.
OUTPUT_TRIGGER_LATENCY   = 8
# Generator: output cycle - generator cycle (generator sample 8*c on the output at cycle c + latency,
# cycle 0: capture enable seen by the Capture).
OUTPUT_GENERATOR_LATENCY = 1
# Periodic: first high output cycle after the capture enable (cycle 0).
OUTPUT_PERIODIC_LATENCY  = 3

# Output core --------------------------------------------------------------------------------------

def run_output(n, setup, events, cycles):
    """Run the Output core: setup {signal: value}, events {cycle: [(signal, value)]}, returns the
    (o, oe_o) values per cycle."""
    dut = Output(n)
    out = []
    def generator():
        for name, value in setup.items():
            yield getattr(dut, name).eq(value)
        for c in range(cycles):
            for name, value in events.get(c, []):
                yield getattr(dut, name).eq(value)
            yield
            out.append(((yield dut.o), (yield dut.oe_o)))
    run_simulation(dut, generator())
    return out

class TestOutputCore(unittest.TestCase):
    def test_gpio_invert_oe(self):
        out = run_output(3, dict(oe=0b101, gpio=0b011, invert=0b100, sel=0), {}, 4)
        self.assertEqual(out[-1], (0b111, 0b101))

    def test_tristate_default(self):
        out = run_output(2, dict(sel=(OUTPUT_PERIODIC << 4) | OUTPUT_PERIODIC, high=2, low=2), {}, 10)
        self.assertTrue(all(oe == 0 for _, oe in out))

    def test_periodic(self):
        # High 2, low 3 (period 5), restarted by start; high 0: always low, low 0: always high.
        out = run_output(1, dict(oe=1, sel=OUTPUT_PERIODIC, high=2, low=3),
            {3: [("start", 1)], 4: [("start", 0)]}, 30)
        bits = [o for o, _ in out]
        self.assertEqual(bits[6:21], [1, 1, 0, 0, 0]*3) # start (cycle 3) + 3.
        out = run_output(1, dict(oe=1, sel=OUTPUT_PERIODIC, high=0, low=3), {3: [("start", 1)], 4: [("start", 0)]}, 20)
        self.assertEqual(sum(o for o, _ in out), 0)
        out = run_output(1, dict(oe=1, sel=OUTPUT_PERIODIC, high=4, low=0), {3: [("start", 1)], 4: [("start", 0)]}, 20)
        self.assertEqual([o for o, _ in out][6:], [1]*14)

    def test_trigger_pulse(self):
        for delay, width in [(0, 1), (0, 4), (7, 3), (100, 16), (3, 255)]:
            with self.subTest(delay=delay, width=width):
                out = run_output(1, dict(oe=1, sel=OUTPUT_TRIGGER, delay=delay, width=width),
                    {10: [("trigger", 1)], 11: [("trigger", 0)]}, 400)
                bits  = [o for o, _ in out]
                first = bits.index(1)
                self.assertEqual(first, 10 + 3 + delay) # Trigger + 2 + delay (+ output register).
                self.assertEqual(bits[first:first + width + 1], [1]*width + [0])
                self.assertEqual(sum(bits), width)

    def test_trigger_no_width(self):
        out = run_output(1, dict(oe=1, sel=OUTPUT_TRIGGER, width=0), {5: [("trigger", 1)], 6: [("trigger", 0)]}, 40)
        self.assertEqual(sum(o for o, _ in out), 0)

    def test_levels_generator(self):
        out = run_output(4, dict(oe=0xf, generator=0b10100110,
            sel=OUTPUT_ARMED | (OUTPUT_CAPTURE << 4) | ((OUTPUT_GENERATOR + 1) << 8) | ((OUTPUT_GENERATOR + 7) << 12),
            armed=1, running=0), {}, 4)
        self.assertEqual(out[-1][0], 0b1101)

# Analyzer -----------------------------------------------------------------------------------------

def run_analyzer(sel, cycles, setup={}, counter=True):
    """Analyzer (cd sys, 1 AUX + CH15 output, 16 channels): base samples fed directly (base sample t = t,
    the captured sample i = 4*i at divider 4), returns the output levels per cycle (cycle 0: enable)."""
    dut = Analyzer(ratio=8, fifo_depth=256, cd="sys", naux=1, output_channels=[15])
    out = []
    def generator():
        yield dut.mode.eq(2)
        yield dut.divider.eq(setup.get("divider", 4)) # 16 channels: divider >= 4.
        yield dut.out_ctrl.eq(0b11)
        yield dut.out_sel.eq(sel)
        for name, value in setup.items():
            if name != "divider":
                yield getattr(dut, name).eq(value)
        yield dut.source.ready.eq(1)
        for _ in range(8):
            yield
        for c in range(cycles):
            yield dut.samples.eq(sum(((8*c + j) & 0xffff) << (16*j) for j in range(8)))
            if c == 0:
                yield dut.enable.eq(1)
            yield
            out.append((yield dut.out_o))
    run_simulation(dut, generator())
    return out

class TestOutputAnalyzer(unittest.TestCase):
    def test_trigger_latency(self):
        # Level trigger on the value 0x0124 (base sample 0x124: cycle 0x24, captured at divider 4).
        value = 0x0124
        out = run_analyzer(OUTPUT_TRIGGER, 80, dict(out_pulse=3 << 24, trigger_enable=1,
            trigger_one=value, trigger_zero=~value & 0xffff, limit=0))
        bits  = [o & 1 for o in out]
        first = bits.index(1)
        self.assertEqual(first - (value >> 3), OUTPUT_TRIGGER_LATENCY)
        self.assertEqual(sum(bits), 3)

    def test_generator_latency(self):
        # Generator CH0/CH4 on AUX0/CH15 (cycle rate: sample 0 of each generator cycle).
        out = run_analyzer(OUTPUT_GENERATOR | ((OUTPUT_GENERATOR + 4) << 4), 3000, dict(pattern=2))
        for k, ch in [(0, 0), (1, 4)]:
            got = [(o >> k) & 1 for o in out]
            L   = OUTPUT_GENERATOR_LATENCY
            ref = [(generator_sample(8*c) >> ch) & 1 for c in range(len(out) - L)]
            bad = next((c for c in range(len(ref)) if got[L + c] != ref[c]), None)
            self.assertIsNone(bad, f"generator CH{ch}: first mismatch at cycle {bad}")
            self.assertGreater(sum(ref), 0)

    def test_periodic_phase(self):
        # Periodic restarted with the capture: first high cycle at OUTPUT_PERIODIC_LATENCY.
        out  = run_analyzer(OUTPUT_PERIODIC, 60, dict(out_high=3, out_low=7))
        bits = [o & 1 for o in out]
        L    = OUTPUT_PERIODIC_LATENCY
        self.assertEqual(bits[L:L + 30], [1, 1, 1, 0, 0, 0, 0, 0, 0, 0]*3)

    def test_capture_armed(self):
        # Capture: high from the start; armed: high until the trigger.
        value = 0x0400
        out = run_analyzer(OUTPUT_CAPTURE | (OUTPUT_ARMED << 4), 300, dict(trigger_enable=1,
            trigger_one=value, trigger_zero=~value & 0xffff, limit=64))
        cap   = [o & 1 for o in out]
        armed = [(o >> 1) & 1 for o in out]
        self.assertEqual(cap[:3], [0, 0, 0])
        self.assertIn(1, cap)
        a0, a1 = armed.index(1), len(armed) - armed[::-1].index(1)
        self.assertLess(a0, value >> 3)
        self.assertGreaterEqual(a1, value >> 3)
        self.assertEqual(armed[a1:], [0]*(len(armed) - a1))

if __name__ == "__main__":
    unittest.main()
