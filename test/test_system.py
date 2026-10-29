#
# This file is part of SLogic16U3.
#
# Copyright (c) 2026 Florent Kermarrec <florent@enjoy-digital.fr>
# SPDX-License-Identifier: BSD-2-Clause

"""System: USB3 host model <-> USB3 Device Core (PHY stand-in, 64-bit Link Layer) <-> SLogic SoC (same
SoC as the hardware target: Analyzer, Flash, fixed CSR map), CSR accesses over EP0 (CommUSB) and
captures over the Bulk IN endpoint (Migen sim)."""

import unittest

from migen import *

from litex.gen import *

from litex.build.generic_platform import GenericPlatform, Pins

from test.usb3_models import host as usb3_host, device as usb3_device
from usb3_core.host_model import PHYStandIn, setup_packet

PyHost             = usb3_host.PyHost
pyhost_link_init   = usb3_host.pyhost_link_init
pyhost_control_in  = usb3_host.pyhost_control_in
pyhost_control_out = usb3_host.pyhost_control_out
pyhost_bulk_in     = usb3_host.pyhost_bulk_in
PairsPHY           = usb3_device.PairsPHY

from slogic_16u3 import SLogicBaseSoC
from gateware.analyzer import ANALYZER_MAGIC

from test.common import capture_reference, unpack

# Sim SoC ------------------------------------------------------------------------------------------

class SimSoC(SLogicBaseSoC):
    def __init__(self):
        platform = GenericPlatform("sim", [("dummy", 0, Pins(1))])
        SLogicBaseSoC.__init__(self, platform, clk_freq=125e6, ident="SLogic16U3 Sim")
        self.phy = phy = PHYStandIn()
        self.samples = Signal(8*16)
        self.add_slogic(PairsPHY(phy, self), self.samples, sample_cd="sys", fifo_depth=256)
        self.finalize()

# CSR Helpers --------------------------------------------------------------------------------------

def csr_addr(soc, name):
    """CSR byte address (fixed map) from the SoC CSR regions."""
    bank, reg = name.split("_", 1)
    region = soc.csr.regions[bank]
    offset = 0
    for csr in region.obj:
        if csr.name in (name, reg):
            return soc.mem_map["csr"] + region.origin + offset
        offset += 4*((csr.size + 31)//32)
    raise KeyError(name)

def wb_read(host, addr, n=1):
    data = yield from pyhost_control_in(host,
        setup_packet(0xc3, 0x00, addr & 0xffff, (addr >> 16) & 0xffff, 4*n))
    return [int.from_bytes(bytes(data[i:i+4]), "little") for i in range(0, len(data), 4)]

def wb_write(host, addr, words):
    data = [b for w in words for b in w.to_bytes(4, "little")]
    return (yield from pyhost_control_out(host,
        setup_packet(0x43, 0x00, addr & 0xffff, (addr >> 16) & 0xffff, 4*len(words)), data))

def csr_read(soc, host, name, n=1):
    words = yield from wb_read(host, csr_addr(soc, name), n)
    value = 0
    for w in words: # LiteX CSRs: most significant word first.
        value = (value << 32) | w
    return value

def csr_write(soc, host, name, value, n=1):
    words = [(value >> (32*(n - 1 - i))) & 0xffffffff for i in range(n)]
    return (yield from wb_write(host, csr_addr(soc, name), words))

# Tests --------------------------------------------------------------------------------------------

class TestSystem(unittest.TestCase):
    def run_system(self, scenario):
        soc  = SimSoC()
        host = PyHost(soc.phy)
        def main():
            yield from pyhost_link_init(host)
            yield from scenario(soc, host)
        run_simulation(soc, [host.driver(), host.monitor(), main()])

    def test_info(self):
        def scenario(soc, host):
            self.assertEqual((yield from csr_read(soc, host, "analyzer_magic")), ANALYZER_MAGIC)
            config = yield from csr_read(soc, host, "analyzer_config")
            self.assertEqual(config & 0xff, 16)
            self.assertEqual((config >> 8) & 0xff, 8)
            self.assertEqual((yield from csr_read(soc, host, "analyzer_base_rate")), 800000000)
            # Read/write a CSR (divider).
            self.assertTrue((yield from csr_write(soc, host, "analyzer_divider", 25)))
            self.assertEqual((yield from csr_read(soc, host, "analyzer_divider")), 25)
        self.run_system(scenario)

    def test_pattern_capture(self):
        # 16 channels, divider 4 (200MS/s), counter pattern, 1000 samples (2000 bytes: 2 packets).
        limit, divider, mode = 1000, 4, 2
        def scenario(soc, host):
            yield from csr_write(soc, host, "analyzer_divider", divider)
            yield from csr_write(soc, host, "analyzer_limit", limit, n=2)
            yield from csr_write(soc, host, "analyzer_control", (mode << 4) | (1 << 8) | 1)
            packets, _ = yield from pyhost_bulk_in(host, ept=1, count=2)
            data = [b for p in packets for b in p]
            words = [int.from_bytes(bytes(data[i:i+8]), "little") for i in range(0, len(data), 8)]
            ref = capture_reference(list(range(65536)), divider, mode, limit)
            self.assertEqual(unpack(words, mode, limit), ref)
            status = yield from csr_read(soc, host, "analyzer_status")
            self.assertEqual(status & 0b1110, 0b0010) # done, no overflow/error.
            self.assertEqual((yield from csr_read(soc, host, "analyzer_count", n=2)), limit)
        self.run_system(scenario)
