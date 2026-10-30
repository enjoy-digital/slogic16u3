#
# This file is part of SLogic16U3.
#
# Copyright (c) 2026 Florent Kermarrec <florent@enjoy-digital.fr>
# SPDX-License-Identifier: BSD-2-Clause

"""Pattern Generator: samples against the reference model (alone and through the Capture, restart)."""

import unittest

from migen import *

from litex.gen import *

from gateware.capture   import Capture
from gateware.generator import PatternGenerator, generator_sample

from test.common import capture_reference, unpack

RATIO = 8

# DUT ----------------------------------------------------------------------------------------------

class CaptureGenerator(LiteXModule):
    def __init__(self):
        self.capture   = capture   = Capture(ratio=RATIO)
        self.generator = generator = PatternGenerator()
        self.comb += [
            generator.enable.eq(capture.enable),
            capture.generator.eq(generator.word),
        ]

# Tests --------------------------------------------------------------------------------------------

class TestGenerator(unittest.TestCase):
    def test_samples_restart(self):
        # Word of the first enabled cycle = samples 0-7, then on; restarted by enable low.
        dut   = PatternGenerator()
        words = []
        def gen():
            for _ in range(3):
                yield
            for run, cycles in [(0, 3000), (1, 300)]:
                yield dut.enable.eq(1)
                for c in range(cycles):
                    yield
                    words.append((run, c, (yield dut.word)))
                yield dut.enable.eq(0)
                for _ in range(5):
                    yield
        # The word is read one cycle after enable is set: it holds the enabled cycle's samples.
        run_simulation(dut, gen())
        for run, c, w in words:
            for j in range(RATIO):
                self.assertEqual((w >> (16*j)) & 0xffff, generator_sample(8*c + j), f"run {run} cycle {c}")

    def test_capture(self):
        # Through the Capture (decimation, channel modes, packing): sample i = generator sample i*divider.
        for mode, divider, limit in [(0, 1, 2000), (1, 2, 1500), (2, 4, 1200), (2, 25, 400), (0, 3, 800)]:
            with self.subTest(mode=mode, divider=divider):
                dut   = CaptureGenerator()
                cap   = dut.capture
                words = []
                def gen():
                    yield cap.mode.eq(mode)
                    yield cap.divider.eq(divider)
                    yield cap.limit.eq(limit)
                    yield cap.pattern.eq(2)
                    yield cap.source.ready.eq(1)
                    for _ in range(4):
                        yield
                    yield cap.enable.eq(1)
                    for _ in range(20000):
                        yield
                        if (yield cap.source.valid):
                            words.append((yield cap.source.data))
                            if (yield cap.source.last):
                                break
                run_simulation(dut, gen())
                total = limit*divider + 64
                ref   = capture_reference([generator_sample(t) for t in range(total)], divider, mode, limit)
                self.assertEqual(unpack(words, mode, limit), ref)

if __name__ == "__main__":
    unittest.main()
