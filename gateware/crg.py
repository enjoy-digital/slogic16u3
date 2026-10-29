#
# This file is part of SLogic16U3.
#
# Copyright (c) 2026 Florent Kermarrec <florent@enjoy-digital.fr>
# SPDX-License-Identifier: BSD-2-Clause

"""CRG: clocks/resets of the SLogic16U3 (no fabric clock input on the board).

- por: OSCA (internal oscillator, ~51.8MHz), power-on reset (the GW5A PLLs need a long reset).
- sys: from OSCA through a PLL (USB3 Core/SoC, GTR12 control/UPAR: running before the SerDes).
- sample/sample_fast: sample clocks (IDES8 PCLK/FCLK, FCLK/4 through a CLKDIV) from a PLL fed by the
  GTR12 TX clock (sample_clk_source="serdes": 125MHz oscillator, accurate samplerate) or by OSCA
  ("osc": no SerDes dependency, internal oscillator accuracy).
"""

from migen import *
from migen.genlib.resetsync import AsyncResetSynchronizer

from litex.gen import *

from litex.soc.cores.clock.gowin_gw5a import GW5APLL

# Constants ----------------------------------------------------------------------------------------

OSCA_FREQ     = 207.32e6 # Nominal OSCA base frequency.
OSCA_FREQ_DIV = 4

# CRG ----------------------------------------------------------------------------------------------

class CRG(LiteXModule):
    def __init__(self, platform, sys_clk_freq=62.5e6):
        self.platform = platform
        self.rst      = Signal()
        self.cd_por   = ClockDomain(reset_less=True)
        self.cd_sys   = ClockDomain()

        # # #

        # OSCA / Power-on Reset.
        osc      = Signal()
        osc_freq = OSCA_FREQ/OSCA_FREQ_DIV
        osc.attr.add(("syn_keep", 1)) # Clock object of the timing constraints (PLL source).
        self.specials += Instance("OSCA",
            p_FREQ_DIV = OSCA_FREQ_DIV,
            i_OSCEN    = 1,
            o_OSCOUT   = osc,
        )
        self.osc      = osc
        self.osc_freq = osc_freq
        por_count     = Signal(16, reset=2**16 - 1)
        self.por_done = por_done = Signal()
        self.comb += [
            self.cd_por.clk.eq(osc),
            por_done.eq(por_count == 0),
        ]
        self.sync.por += If(~por_done, por_count.eq(por_count - 1))
        platform.add_period_constraint(osc, 1e9/osc_freq)

        # Sys PLL.
        self.sys_pll = sys_pll = GW5APLL(devicename=platform.devicename, device=platform.device, name="sys_pll")
        self.comb += sys_pll.reset.eq(~por_done | self.rst)
        sys_pll.register_clkin(osc, osc_freq)
        sys_pll.create_clkout(self.cd_sys, sys_clk_freq, margin=2e-2)
        sys_pll.add_generated_clock_constraints(platform)

    def add_sample_clk(self, sample_clk_freq=100e6, source="serdes", serdes_clk=None, serdes_clk_freq=125e6):
        """Sample clocks: PLL (FCLK, sample_fast) + CLKDIV (PCLK = FCLK/4, sample), from the GTR12 TX
        clock (source="serdes": serdes_clk, the Signal of the TX clock domain) or OSCA ("osc")."""
        assert source in ["serdes", "osc"]
        platform = self.platform
        self.cd_sample      = ClockDomain()
        self.cd_sample_fast = ClockDomain(reset_less=True)
        self.sample_pll = sample_pll = GW5APLL(devicename=platform.devicename, device=platform.device, name="sample_pll")
        if source == "serdes":
            assert serdes_clk is not None
            sample_pll.register_clkin(serdes_clk, serdes_clk_freq)
        else:
            sample_pll.register_clkin(self.osc, self.osc_freq)
        self.comb += sample_pll.reset.eq(~self.por_done | self.rst)
        sample_pll.create_clkout(self.cd_sample_fast, 4*sample_clk_freq, with_reset=False)
        sample_pll.add_generated_clock_constraints(platform)
        self.cd_sample.clk.attr.add(("syn_keep", 1))
        self.specials += [
            Instance("CLKDIV",
                p_DIV_MODE = "4",
                i_HCLKIN   = self.cd_sample_fast.clk,
                i_RESETN   = sample_pll.locked,
                i_CALIB    = 0,
                o_CLKOUT   = self.cd_sample.clk,
            ),
            AsyncResetSynchronizer(self.cd_sample, ~sample_pll.locked),
        ]
        platform.add_period_constraint(self.cd_sample.clk, 1e9/sample_clk_freq)
