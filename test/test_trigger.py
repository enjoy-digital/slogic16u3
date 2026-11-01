#
# This file is part of SLogic16U3.
#
# Copyright (c) 2026 Florent Kermarrec <florent@enjoy-digital.fr>
# SPDX-License-Identifier: BSD-2-Clause

"""Hardware trigger: trigger match (edge/level/pattern), trigger position, pre-trigger window and
post-trigger limit of the Analyzer against a reference model (Migen sim, single clock domain)."""

import random
import unittest

from migen import *

from gateware.analyzer import Analyzer, PRETRIGGER_MARGIN

from test.common import MODE_BITS, capture_reference, unpack

# Helpers ------------------------------------------------------------------------------------------

RATIO      = 8
FIFO_DEPTH = 256

def trigger_reference(samples, zero=0, one=0, rising=0, falling=0):
    """Index of the first matching captured sample (sigrok semantics), None if no match."""
    edges = rising | falling
    for i, s in enumerate(samples):
        if (s & zero) or (~s & one):
            continue
        if edges:
            if i == 0:
                continue
            p  = samples[i - 1]
            ok = (rising & ~p & s) | (falling & p & ~s)
            if edges & ~ok:
                continue
        return i
    return None

def window_reference(samples, t, divider, mode, pre_words, limit):
    """Expected stream samples: from word max(0, W - pre_words) to the trigger cycle start + limit."""
    spw   = 64//MODE_BITS[mode]
    first = max(0, t//spw - pre_words)*spw
    cycle = (t*divider)//RATIO                # Base cycle of the trigger sample.
    start = -(-(cycle*RATIO)//divider)        # First captured sample of that cycle.
    end   = start + limit if limit else len(samples)
    return first, samples[first:end]

def toggling_inputs(prng, n, rate):
    """16-bit samples, each channel toggling with probability rate per base sample."""
    value, out = prng.randrange(2**16), []
    for _ in range(n):
        for c in range(16):
            if prng.random() < rate:
                value ^= 1 << c
        out.append(value)
    return out

def run_trigger(divider, mode, limit, pre_words, cond, inputs, ready_rate=1.0, stop_after=None,
    seed=0, max_cycles=6000):
    prng   = random.Random(seed)
    dut    = Analyzer(ratio=RATIO, fifo_depth=FIFO_DEPTH, cd="sys")
    words  = []
    status = {}
    def generator():
        yield dut.mode.eq(mode)
        yield dut.divider.eq(divider)
        yield dut.limit.eq(limit)
        yield dut.pretrigger.eq(pre_words)
        yield dut.trigger_enable.eq(cond is not None)
        for name, value in (cond or {}).items():
            yield getattr(dut, "trigger_" + name).eq(value)
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
        for _ in range(4):
            yield
        for name in ["done", "overflow", "triggered", "trigger_count", "count"]:
            status[name] = (yield getattr(dut, name))
    run_simulation(dut, generator())
    return words, status

# Tests --------------------------------------------------------------------------------------------

class TestTrigger(unittest.TestCase):
    def check(self, divider, mode, limit, pre_words, cond, seed=0, rate=0.002, ready_rate=1.0):
        prng     = random.Random(seed)
        ncycles  = 6000
        inputs   = toggling_inputs(prng, RATIO*ncycles, rate)
        captured = capture_reference(inputs, divider, mode)
        t        = trigger_reference(captured, **cond)
        words, status = run_trigger(divider, mode, limit, pre_words, cond, inputs,
            ready_rate=ready_rate, seed=seed, max_cycles=ncycles - 16)
        self.assertIsNotNone(t)
        self.assertTrue(status["done"])
        self.assertFalse(status["overflow"])
        self.assertTrue(status["triggered"])
        self.assertEqual(status["trigger_count"], t)
        first, ref = window_reference(captured, t, divider, mode, pre_words, limit)
        self.assertEqual(status["count"], first + len(ref))
        self.assertEqual(unpack(words, mode, len(ref)), ref)
        return t, first

    def test_conditions(self):
        # Rising, falling, any edge, level, pattern and pattern + edge (CH0-CH3: all the modes).
        conds = [
            dict(rising=0x0001),
            dict(falling=0x0004),
            dict(rising=0x0002, falling=0x0002),
            dict(one=0x0005, zero=0x000a),
            dict(one=0x0003, falling=0x0004),
        ]
        for mode, divider in [(0, 1), (1, 2), (2, 4), (0, 3), (2, 25)]:
            for i, cond in enumerate(conds):
                with self.subTest(mode=mode, divider=divider, cond=cond):
                    self.check(divider, mode, limit=200, pre_words=16, cond=cond, seed=i)

    def test_high_channels(self):
        # Conditions on CH8-CH15 (16 channels), CH4-CH7 (8 channels).
        self.check(4, 2, limit=100, pre_words=8, cond=dict(rising=0x8000, one=0x0100), seed=3)
        self.check(2, 1, limit=100, pre_words=8, cond=dict(falling=0x0080), seed=4)

    def test_pretrigger_window(self):
        # Full window (late trigger), short window (early trigger), no pre-trigger.
        for pre_words in [0, 1, 40, FIFO_DEPTH - 2*PRETRIGGER_MARGIN]:
            with self.subTest(pre_words=pre_words):
                t, first = self.check(1, 0, limit=300, pre_words=pre_words, cond=dict(rising=0x0008),
                    seed=10, rate=0.0002)
                self.assertGreater(t, 16*pre_words) # Late trigger: full window.
                self.assertEqual(t - first, t % 16 + 16*pre_words)
        # Early trigger: level matching the first sample.
        prng   = random.Random(11)
        inputs = toggling_inputs(prng, RATIO*2000, 0.001)
        cond   = dict(one=inputs[0] & 0xf, zero=~inputs[0] & 0xf)
        t, first = self.check(1, 0, limit=64, pre_words=32, cond=cond, seed=11, rate=0.001)
        self.assertEqual((t, first), (0, 0))

    def test_backpressure(self):
        self.check(8, 2, limit=400, pre_words=24, cond=dict(rising=0x0010), seed=20, ready_rate=0.6)

    def test_unlimited_post(self):
        # limit 0: streamed until stopped.
        prng     = random.Random(30)
        inputs   = toggling_inputs(prng, RATIO*3000, 0.002)
        captured = capture_reference(inputs, 2, 1)
        cond     = dict(rising=0x0001)
        t        = trigger_reference(captured, **cond)
        words, status = run_trigger(2, 1, 0, 8, cond, inputs, stop_after=2000, max_cycles=2900)
        self.assertTrue(status["triggered"])
        self.assertEqual(status["trigger_count"], t)
        first = max(0, t//8 - 8)*8
        n     = status["count"] - first
        self.assertGreater(n, 0)
        self.assertEqual(unpack(words, 1, n), captured[first:first + n])

    def test_no_match(self):
        # Never matching: nothing streamed, stop ends the capture.
        inputs = [0]*(RATIO*1000)
        words, status = run_trigger(1, 0, 100, 8, dict(one=0x0001), inputs, stop_after=500,
            max_cycles=800)
        self.assertEqual(words, [])
        self.assertFalse(status["triggered"])
        self.assertFalse(status["overflow"])

    def test_disabled(self):
        # Trigger disabled: unchanged capture.
        prng     = random.Random(40)
        inputs   = [prng.randrange(2**16) for _ in range(RATIO*1000)]
        words, status = run_trigger(3, 0, 500, 8, None, inputs)
        self.assertEqual(status["count"], 500)
        self.assertEqual(unpack(words, 0, 500), capture_reference(inputs, 3, 0, 500))

    def test_clock_domains(self):
        # Capture in the sample clock domain, window in sys (status CDC, capture generations): two
        # successive triggered captures (the second must not use the trigger status of the first).
        prng   = random.Random(50)
        inputs = toggling_inputs(prng, RATIO*4000, 0.002)
        dut    = ClockDomainsRenamer({"sample": "sample"})(Analyzer(ratio=RATIO, fifo_depth=FIFO_DEPTH,
            cd="sample"))
        dut.clock_domains.cd_sample = ClockDomain("sample")
        results = []
        state   = {"cycle": 0, "run": False}
        @passive
        def sample_gen():
            while True:
                c = (state["cycle"] if state["run"] else 0) % 4000
                yield dut.samples.eq(sum(inputs[RATIO*c + i] << (16*i) for i in range(RATIO)))
                if state["run"]:
                    state["cycle"] += 1
                yield
        def sys_gen():
            for divider, mode, cond in [(1, 0, dict(rising=0x0002)), (2, 1, dict(falling=0x0040))]:
                yield dut.mode.eq(mode)
                yield dut.divider.eq(divider)
                yield dut.limit.eq(150)
                yield dut.pretrigger.eq(12)
                yield dut.trigger_enable.eq(1)
                for name in ["zero", "one", "rising", "falling"]:
                    yield getattr(dut, "trigger_" + name).eq(cond.get(name, 0))
                for _ in range(20):
                    yield
                state["cycle"], state["run"] = 0, False
                yield dut.enable.eq(1)
                yield dut.source.ready.eq(1)
                words = []
                for _ in range(20000):
                    yield
                    # Samples restart with the capture (first word with enable seen in the domain).
                    if not state["run"] and (yield dut.capture.running):
                        state["run"] = True
                    if (yield dut.source.valid):
                        words.append((yield dut.source.data))
                        if (yield dut.source.last):
                            break
                for _ in range(40):
                    yield
                results.append((divider, mode, cond, words,
                    (yield dut.triggered), (yield dut.trigger_count), (yield dut.count)))
                yield dut.enable.eq(0)
                for _ in range(40):
                    yield
        run_simulation(dut, {"sys": [sys_gen()], "sample": [sample_gen()]},
            clocks={"sys": 16, "sample": 10})
        self.assertEqual(len(results), 2)
        for divider, mode, cond, words, triggered, trigger_count, count in results:
            self.assertTrue(triggered)
            self.assertGreater(len(words), 0)
            # Samples are aligned to the capture start up to the run detection: check the window
            # self-consistency (trigger sample matches, stream = captured samples from first).
            spw   = 64//MODE_BITS[mode]
            first = max(0, trigger_count//spw - 12)*spw
            n     = count - first
            got   = unpack(words, mode, n)
            self.assertEqual(len(got), n)
            t = trigger_count - first
            bit = (cond.get("rising", 0) | cond.get("falling", 0))
            if cond.get("rising"):
                self.assertTrue((got[t] & bit) and not (got[t - 1] & bit))
            else:
                self.assertTrue(not (got[t] & bit) and (got[t - 1] & bit))
            self.assertIsNone(trigger_reference(got[:t], **cond) if t > 1 else None)

if __name__ == "__main__":
    unittest.main()
