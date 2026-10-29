#
# This file is part of SLogic16U3.
#
# Copyright (c) 2026 Florent Kermarrec <florent@enjoy-digital.fr>
# SPDX-License-Identifier: BSD-2-Clause

"""Sampler: logic analyzer inputs deserialized by the GW5A IDES8 (8 samples per sample clock cycle).

Each input is sampled on both edges of the fast clock (FCLK, ex: 400MHz: 800MS/s) and deserialized
on the sample clock (PCLK = FCLK/4, ex: 100MHz, from a CLKDIV of FCLK in the CRG): samples[16*j + i]
is sample j (j = 0: first in time, IDES8 Q0, checked with the Gowin simulation model) of channel i.
"""

from migen import *

from litex.gen import *

# GW5A Sampler -------------------------------------------------------------------------------------

class GW5ASampler(LiteXModule):
    def __init__(self, pads, nchannels=16, ratio=8, cd="sample", cd_fast="sample_fast"):
        assert ratio == 8
        self.samples = Signal(ratio*nchannels) # o: Samples (cd clock domain).

        # # #

        q = [Signal(ratio) for _ in range(nchannels)]
        for i in range(nchannels):
            self.specials += Instance("IDES8",
                i_D      = pads[i],
                i_FCLK   = ClockSignal(cd_fast),
                i_PCLK   = ClockSignal(cd),
                i_CALIB  = 0,
                i_RESET  = ResetSignal(cd),
                **{f"o_Q{j}": q[i][j] for j in range(ratio)}
            )

        # Samples (sample-major), registered.
        sync = getattr(self.sync, cd)
        sync += self.samples.eq(Cat(*[q[i][j] for j in range(ratio) for i in range(nchannels)]))
