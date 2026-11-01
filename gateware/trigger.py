#
# This file is part of SLogic16U3.
#
# Copyright (c) 2026 Florent Kermarrec <florent@enjoy-digital.fr>
# SPDX-License-Identifier: BSD-2-Clause

"""Trigger: single stage hardware trigger on the captured (decimated) samples, sigrok semantics.

Each channel has 4 condition bits (16-bit masks, all the channels ANDed):

- zero   : channel low.
- one    : channel high.
- rising : low on the previous sample, high on this one.
- falling: high on the previous sample, low on this one (rising + falling: any edge).

Channels without condition are don't care. This gives edge, level and pattern triggers, and their
combinations (ex: CH1 falling while CH6-CH15 match a pattern).

The match is evaluated on the compacted lanes of the Capture (lanes[0:k] valid, lane j: sample j of the
cycle, lanes sized for the valid configurations: channels above a lane width are don't care). The
previous sample of lane j is lane j-1, the one of lane 0 the last valid lane of the previous cycle (no
edge can match on the first captured sample). Output: `match` (registered, one bit per lane, valid
lanes only), the first set bit is the trigger sample.
"""

from migen import *

from litex.gen import *

# Trigger ------------------------------------------------------------------------------------------

class Trigger(LiteXModule):
    def __init__(self, lanes, nchannels=16):
        n = len(lanes)
        self.k       = Signal(4)                          # i: Valid lanes (lanes[0:k]).
        self.run     = Signal()                           # i: Capture data running (low: previous sample cleared).
        self.zero    = Signal(nchannels, reset_less=True) # i: Conditions (quasi-static, channel mode masked).
        self.one     = Signal(nchannels, reset_less=True) # i
        self.rising  = Signal(nchannels, reset_less=True) # i
        self.falling = Signal(nchannels, reset_less=True) # i
        self.match   = Signal(n)                          # o: Matching lanes (registered, valid lanes only).

        # # #

        # Previous sample (last valid lane of the previous cycle).
        last      = Signal(nchannels, reset_less=True)
        have_last = Signal()
        self.sync += [
            If(~self.run,
                have_last.eq(0)
            ).Elif(self.k != 0,
                have_last.eq(1),
                Case(self.k, {i + 1: last.eq(lanes[i]) for i in range(n)})
            )
        ]

        # Match: no failing condition on the channels.
        edges = Signal(nchannels)
        self.comb += edges.eq(self.rising | self.falling)
        for j in range(n):
            s    = Signal(nchannels)
            p    = Signal(nchannels)
            fail = Signal(nchannels)
            ok   = Signal()
            self.comb += [
                s.eq(lanes[j]),
                p.eq(last if j == 0 else lanes[j - 1]),
                fail.eq(
                    (self.zero & s) |
                    (self.one & ~s) |
                    (edges & ~((self.rising & ~p & s) | (self.falling & p & ~s)))
                ),
                ok.eq((fail == 0) & (have_last | (edges == 0)) if j == 0 else (fail == 0)),
            ]
            self.sync += self.match[j].eq(ok & (self.k > j) & self.run)
