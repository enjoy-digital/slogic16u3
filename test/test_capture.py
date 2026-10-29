#
# This file is part of SLogic16U3.
#
# Copyright (c) 2026 Florent Kermarrec <florent@enjoy-digital.fr>
# SPDX-License-Identifier: BSD-2-Clause

"""Capture: decimation/channel modes/packing/limit/overflow against the reference model (Migen sim)."""

import random
import unittest

from migen import *

from gateware.capture import Capture

from test.common import capture_reference, unpack

# Helpers ------------------------------------------------------------------------------------------

RATIO = 8

def run_capture(divider, mode, limit, pattern=0, seed=0, ready_rate=1.0, stop_after=None,
    max_cycles=4000):
    """Run a capture, return (samples fed, words received, final status)."""
    prng    = random.Random(seed)
    dut     = Capture(ratio=RATIO)
    inputs  = [prng.randrange(2**16) for _ in range(RATIO*max_cycles)]
    words   = []
    status  = {}
    def generator():
        yield dut.mode.eq(mode)
        yield dut.divider.eq(divider)
        yield dut.limit.eq(limit)
        yield dut.pattern.eq(pattern)
        for _ in range(4):
            yield
        cycle = 0
        while True:
            yield dut.samples.eq(sum(inputs[RATIO*cycle + i] << (16*i) for i in range(RATIO)))
            if cycle == 0:
                yield dut.enable.eq(1)
            if (stop_after is not None) and (cycle == stop_after):
                yield dut.enable.eq(0)
            yield dut.source.ready.eq(prng.random() < ready_rate)
            yield
            cycle += 1
            if (yield dut.source.valid) and (yield dut.source.ready):
                words.append((yield dut.source.data))
                if (yield dut.source.last):
                    break
            if cycle >= max_cycles:
                break
        for _ in range(2):
            yield
        for name in ["done", "overflow", "error", "count", "words", "running"]:
            status[name] = (yield getattr(dut, name))
    run_simulation(dut, generator())
    return inputs, words, status

# Tests --------------------------------------------------------------------------------------------

class TestCapture(unittest.TestCase):
    def check_capture(self, divider, mode, limit, pattern=0, seed=0):
        inputs, words, status = run_capture(divider, mode, limit, pattern=pattern, seed=seed)
        self.assertTrue(status["done"])
        self.assertFalse(status["overflow"])
        self.assertFalse(status["error"])
        self.assertEqual(status["count"], limit)
        self.assertEqual(status["words"], len(words))
        if pattern:
            # Counter: sample i of the capture = i (16-bit).
            ref = capture_reference([i & 0xffff for i in range(len(inputs))], divider, mode, limit)
        else:
            ref = capture_reference(inputs, divider, mode, limit)
        self.assertEqual(unpack(words, mode, limit), ref)

    def test_dividers_modes(self):
        # Valid configurations (<= 32 bits/cycle): 4ch from /1, 8ch from /2, 16ch from /4.
        for mode, dmin in [(0, 1), (1, 2), (2, 4)]:
            for divider in [dmin, 3, 5, 7, 8, 9, 10, 16, 25]:
                if divider < dmin:
                    continue
                with self.subTest(mode=mode, divider=divider):
                    self.check_capture(divider, mode, limit=97, seed=divider)

    def test_counter_pattern(self):
        for mode, divider in [(0, 1), (1, 2), (2, 4), (2, 5), (2, 100)]:
            with self.subTest(mode=mode, divider=divider):
                self.check_capture(divider, mode, limit=200, pattern=1)

    def test_limit_alignment(self):
        # Limits ending on/off word boundaries (16 samples of 4 bits = 1 word).
        for limit in [1, 15, 16, 17, 32, 33]:
            with self.subTest(limit=limit):
                self.check_capture(1, 0, limit)

    def test_too_wide(self):
        # 16 channels at /1: 128 bits/cycle, invalid configuration.
        _, _, status = run_capture(1, 2, limit=100)
        self.assertTrue(status["error"])
        self.assertTrue(status["done"])

    def test_overflow(self):
        # Sink rarely ready: data lost, capture stopped with overflow set.
        _, words, status = run_capture(1, 0, limit=10000, ready_rate=0.1)
        self.assertTrue(status["overflow"])
        self.assertTrue(status["done"])
        self.assertLess(status["count"], 10000)

    def test_stop(self):
        # No limit: stopped by the host (enable falling), samples counted until the stop.
        inputs, words, status = run_capture(2, 1, limit=0, stop_after=50)
        self.assertTrue(status["done"])
        self.assertFalse(status["overflow"])
        count = status["count"]
        self.assertGreater(count, 0)
        self.assertEqual(unpack(words, 1, count), capture_reference(inputs, 2, 1, count))
