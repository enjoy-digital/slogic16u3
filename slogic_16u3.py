#!/usr/bin/env python3

#
# This file is part of SLogic16U3.
#
# Copyright (c) 2026 Florent Kermarrec <florent@enjoy-digital.fr>
# SPDX-License-Identifier: BSD-2-Clause

"""SLogic16U3: open LiteX gateware for the Sipeed SLogic16U3 USB3 logic analyzer.

    CH0-CH15 -> Sampler (IDES8) -> Analyzer (Capture, FIFO) -> USB3 Bulk IN (EP1) -> Host
                                    CSRs <- USB3 EP0 (Wishbone bridge) <- Host
    SPI Flash <- Flash core (remote update)

USB3: GTR12 Q0 lane 0 (125MHz REFCLK0) with USB3-PIPE, PIPE on the GTR12 TX clock (125MHz), LTSSM/Link
Layer/Core/SoC in sys (62.5MHz, 64-bit Link Layer words).
"""

import os
import re
import argparse

from migen import *

from litex.gen import *

from litex.build.generic_platform import Pins, IOStandard

from litex.soc.integration.soc      import SoCRegion
from litex.soc.integration.soc_core import SoCMini
from litex.soc.integration.builder  import Builder

from litex_boards.platforms import sipeed_slogic16u3

from gateware.crg      import CRG
from gateware.sampler  import GW5ASampler
from gateware.analyzer import Analyzer
from gateware.flash    import Flash
from gateware.usb3     import USB3Device, USB3_VID, USB3_PID

# IOs ----------------------------------------------------------------------------------------------

_mspi_io = [
    # Configuration SPI Flash (MSPI pins, functions mapped at runtime by the Flash core).
    ("mspi", 0, Pins("K13 K14 L14 L13 M14 M13"), IOStandard("LVCMOS33")),
]

# Constants ----------------------------------------------------------------------------------------

FLASH_BUFFER_BASE = 0x20000000

# SLogic16U3 Base SoC ------------------------------------------------------------------------------

class SLogicBaseSoC(SoCMini):
    """SoC shared by the hardware target and the simulations: USB3 Device (EP0 Wishbone bridge, Bulk IN),
    Analyzer, Flash, fixed CSR map."""
    # Fixed CSR map (host software).
    csr_map = {
        "ctrl"     : 0,
        "analyzer" : 1,
        "flash"    : 2,
        "usb3_phy" : 3,
    }

    def add_slogic(self, usb3_phy, samples, sample_cd="sample", base_rate=800e6, mspi_pads=None,
        usb_vid=USB3_VID, usb_pid=USB3_PID, usb_serial="0001", fifo_depth=1024,
        with_analyzer=True, with_flash=True):
        # USB3 Device (EP0 Wishbone bridge, Bulk IN) -----------------------------------------------
        self.usb3 = usb3 = USB3Device(usb3_phy, self.sys_clk_freq,
            data_width = 64,
            vid        = usb_vid,
            pid        = usb_pid,
            serial     = usb_serial)
        self.bus.add_master(name="usb3", master=usb3.bus)

        # Analyzer ---------------------------------------------------------------------------------
        if not with_analyzer:
            self.comb += usb3.sink.valid.eq(0)
        else:
            self.add_analyzer(usb3, samples, sample_cd, base_rate, fifo_depth)

        # Flash (remote update) --------------------------------------------------------------------
        if with_flash:
            self.flash = flash = Flash(mspi_pads)
            flash.add_csr()
            self.bus.add_slave(name="flash_buffer", slave=flash.bus,
                region=SoCRegion(origin=FLASH_BUFFER_BASE, size=flash.buffer_size, cached=False))

    def add_analyzer(self, usb3, samples, sample_cd, base_rate, fifo_depth):
        self.analyzer = analyzer = Analyzer(nchannels=16, ratio=8, data_width=64,
            base_rate  = base_rate,
            fifo_depth = fifo_depth,
            cd         = sample_cd)
        analyzer.add_csr()
        self.comb += [
            analyzer.samples.eq(samples),
            analyzer.source.connect(usb3.sink),
            usb3.flush.eq(analyzer.start),
        ]

# SLogic16U3 SoC -----------------------------------------------------------------------------------

class SLogicSoC(SLogicBaseSoC):
    def __init__(self, platform, sys_clk_freq=62.5e6, sample_clk_freq=100e6, sample_clk_source="serdes",
        usb_vid=USB3_VID, usb_pid=USB3_PID, usb_serial="0001", fifo_depth=512,
        with_analyzer=True, with_flash=True):
        usb3_clk_freq = 125e6

        # SoCMini ----------------------------------------------------------------------------------
        SoCMini.__init__(self, platform, sys_clk_freq, ident="SLogic16U3 LiteX", ident_version=True)

        # USB3 SerDes (GTR12 Q0 lane 0, 125MHz REFCLK0) --------------------------------------------
        from usb3_pipe import GW5USB3SerDes, USB3PHY
        self.comb += [
            platform.request("clk125_oe").eq(1),  # 125MHz oscillator (GTR12 REFCLK0).
            platform.request("usb_pullup").eq(0), # SuperSpeed only.
        ]
        # CRG (the sample clock comes from the GTR12 TX clock: created after the SerDes).
        self.cd_gtr12 = ClockDomain()
        self.crg = CRG(platform, sys_clk_freq)
        self.comb += [
            self.cd_gtr12.clk.eq(self.crg.cd_sys.clk),
            self.cd_gtr12.rst.eq(self.crg.cd_sys.rst),
        ]
        usb3_serdes = GW5USB3SerDes(platform,
            sys_clk        = self.crg.cd_sys.clk,
            sys_clk_freq   = usb3_clk_freq,
            lane           = 0,
            rx_idle_source = "~astat5",
            with_upar      = True,
            upar_clk       = self.crg.cd_sys.clk, # Quad AHB/UPAR clock running from the start.
            xcvr_cd        = "gtr12",
            tx_cdc         = False,               # SerDes logic on the GTR12 TX clock.
            refclk         = "125M",
        )
        self.usb3_serdes = usb3_serdes = ClockDomainsRenamer({"sys": "tx"})(usb3_serdes)
        if with_analyzer:
            self.crg.add_sample_clk(sample_clk_freq,
                source          = sample_clk_source,
                serdes_clk      = usb3_serdes.gtr12.cd_tx.clk,
                serdes_clk_freq = usb3_clk_freq)

        # USB3 PHY (PIPE on the GTR12 TX clock, LTSSM/Link Layer in sys) ---------------------------
        self.usb3_phy = usb3_phy = USB3PHY(
            serdes        = usb3_serdes,
            sys_clk_freq  = sys_clk_freq,
            role          = "device",
            link_words    = 2,
            pipe_cd       = "tx",
            pipe_clk_freq = usb3_clk_freq,
            pipe_rx_depth = 64,  # No Bulk OUT: control/link traffic only from the host.
            with_u1u2     = False)
        self.add_usb3_phy_status(usb3_phy)

        # Sampler / SLogic (USB3 Device, Analyzer, Flash) ------------------------------------------
        samples = Signal(8*16)
        if with_analyzer:
            la_pads = platform.request("la")
            self.sampler = sampler = GW5ASampler(la_pads.ch, nchannels=16, ratio=8)
            samples = sampler.samples
        platform.add_extension(_mspi_io)
        self.add_slogic(usb3_phy, samples,
            sample_cd  = "sample",
            base_rate  = 8*sample_clk_freq,
            mspi_pads  = platform.request("mspi"),
            usb_vid    = usb_vid,
            usb_pid    = usb_pid,
            usb_serial = usb_serial,
            fifo_depth = fifo_depth,
            with_analyzer = with_analyzer,
            with_flash    = with_flash)

        # Led (on: U0, blinking: capture running) --------------------------------------------------
        blink = Signal(24)
        self.sync += blink.eq(blink + 1)
        running = self.analyzer.running if with_analyzer else 0
        self.comb += platform.request("user_led").eq(usb3_phy.u0 & ~(running & blink[23]))

    def add_usb3_phy_status(self, phy):
        """Minimal PHY status: LTSSM state, U0, SerDes ready."""
        from litex.soc.interconnect.csr import CSRStatus, CSRField
        phy.status = CSRStatus(fields=[
            CSRField("ltssm_state", size=len(phy.ltssm_state), offset=0),
            CSRField("u0",          size=1, offset=8),
            CSRField("ready",       size=1, offset=9),
        ])
        self.comb += [
            phy.status.fields.ltssm_state.eq(phy.ltssm_state),
            phy.status.fields.u0.eq(phy.u0),
            phy.status.fields.ready.eq(phy.ready),
        ]

# Gowin Workaround ---------------------------------------------------------------------------------

def gowin_distributed_memories(platform, max_depth=64):
    """Memories mapped to distributed RAMs (syn_ramstyle attribute added to the generated Verilog
    before the Gowin project is created):
    - Initialized memories (identifier, USB3 descriptors: small): Gowin 1.9.10 rejects initialized
      BSRAMs on the GW5AT-15A (PA2128).
    - Shallow memories (depth <= max_depth: FIFOs, CDCs): the 35 BSRAMs (half used per inferred
      SDPB/SDPX9B) are kept for the deep buffers (USB3 packets slots, capture FIFO)."""
    toolchain     = platform.toolchain
    build_project = toolchain.build_project
    attr          = ' /* synthesis syn_ramstyle = "distributed_ram" */'
    def _build_project():
        v_file = f"{toolchain._build_name}.v"
        v      = open(v_file).read()
        v      = re.sub(r"(reg \[\d+:0\] (\w+)\[0:\d+\]);(\s*initial begin\s*\$readmemh\(\"[^\"]+\", \2\);)",
            r"\1" + attr + r";\3", v)
        def shallow(m):
            return m.group(0) if (int(m.group(2)) + 1 > max_depth) or ("syn_ramstyle" in m.group(0)) else m.group(1) + attr + ";"
        v      = re.sub(r"(reg \[\d+:0\] \w+\[0:(\d+)\])[^;\n]*;", shallow, v)
        open(v_file, "w").write(v)
        build_project()
    toolchain.build_project = _build_project

# Build --------------------------------------------------------------------------------------------

def main():
    parser = argparse.ArgumentParser(description="SLogic16U3: open LiteX gateware for the Sipeed SLogic16U3.")
    parser.add_argument("--build",             action="store_true",     help="Build bitstream.")
    parser.add_argument("--load",              action="store_true",     help="Load bitstream (SRAM, JTAG).")
    parser.add_argument("--output-dir",        default="build",         help="Build directory.")
    parser.add_argument("--sys-clk-freq",      default=62.5e6, type=float, help="System clock frequency.")
    parser.add_argument("--sample-clk-freq",   default=100e6,  type=float, help="Sample clock frequency (base samplerate: 8x).")
    parser.add_argument("--sample-clk-source", default="serdes",       help="Sample clock source.", choices=["serdes", "osc"])
    parser.add_argument("--usb-vid",           default=USB3_VID, type=lambda x: int(x, 0), help="USB Vendor ID.")
    parser.add_argument("--usb-pid",           default=USB3_PID, type=lambda x: int(x, 0), help="USB Product ID.")
    parser.add_argument("--usb-serial",        default="0001",          help="USB Serial Number.")
    parser.add_argument("--no-analyzer",       action="store_true",     help="Build without the Analyzer (USB3 bring-up).")
    parser.add_argument("--no-flash",          action="store_true",     help="Build without the Flash core.")
    parser.add_argument("--place-option",      default=2, type=int,     help="Gowin place algorithm.", choices=[0, 1, 2, 3, 4])
    parser.add_argument("--route-option",      default=1, type=int,     help="Gowin route algorithm.", choices=[0, 1, 2])
    args = parser.parse_args()

    platform = sipeed_slogic16u3.Platform(toolchain="gowin")
    platform.toolchain.options["place_option"]        = args.place_option
    platform.toolchain.options["route_option"]        = args.route_option
    platform.toolchain.options["replicate_resources"] = 1
    gowin_distributed_memories(platform)

    soc = SLogicSoC(platform,
        sys_clk_freq      = args.sys_clk_freq,
        sample_clk_freq   = args.sample_clk_freq,
        sample_clk_source = args.sample_clk_source,
        usb_vid           = args.usb_vid,
        usb_pid           = args.usb_pid,
        usb_serial        = args.usb_serial,
        with_analyzer     = not args.no_analyzer,
        with_flash        = not args.no_flash,
    )
    builder = Builder(soc, output_dir=args.output_dir, csr_csv=os.path.join(args.output_dir, "csr.csv"),
        csr_json=os.path.join(args.output_dir, "csr.json"))
    builder.build(run=args.build)

    if args.load:
        prog = platform.create_programmer(kit="openfpgaloader")
        prog.load_bitstream(builder.get_bitstream_filename(mode="sram"))

if __name__ == "__main__":
    main()
