#
# This file is part of SLogic16U3.
#
# Copyright (c) 2026 Florent Kermarrec <florent@enjoy-digital.fr>
# SPDX-License-Identifier: BSD-2-Clause

"""Flash: pin mapping probe (JEDEC ID), erase/program/read through the buffer, with a SPI Flash model
on hidden pads (Migen sim)."""

import itertools
import unittest

from migen import *
from migen.sim import passive

from gateware.flash import Flash

# SPI Flash Model ----------------------------------------------------------------------------------

JEDEC_ID = [0xef, 0x40, 0x18]

class SPIFlashModel:
    """Mode 0 SPI Flash (MSB first): JEDEC ID (9F), RDSR (05), WREN (06), PP (02), SE (20), READ (03)."""
    def __init__(self, pads, size=1 << 20):
        self.pads = pads # {"clk": n, "cs_n": n, "mosi": n, "miso": n}
        self.mem  = bytearray([0xff])*size
        self.wel  = 0
        self.busy = 0

    @passive
    def generator(self, dut):
        clk_d, cs_d = 0, 1
        miso = 1
        cmd  = []
        out  = []
        bitc = 0
        byte = 0
        obit = 0
        while True:
            o  = (yield dut.pads_o)
            oe = (yield dut.pads_oe)
            pin = lambda n: (o >> n) & 1 if (oe >> n) & 1 else 1 # Pull-ups.
            clk, cs_n, mosi = pin(self.pads["clk"]), pin(self.pads["cs_n"]), pin(self.pads["mosi"])
            if cs_n and not cs_d:
                self.end(cmd)
            if not cs_n and cs_d:
                cmd, out, bitc, byte, obit = [], [0xff], 0, 0, 0 # out[k]: response during byte k.
            if not cs_n and clk and not clk_d:
                byte = ((byte << 1) | mosi) & 0xff
                bitc += 1
                if bitc == 8:
                    cmd.append(byte)
                    out += self.response(cmd)
                    bitc = 0
            if not cs_n and not clk and clk_d:
                obit += 1
            # MISO: bit (obit) of the response stream (MSB first), changes on CLK falling edges.
            idx  = obit//8
            miso = (out[idx] >> (7 - obit % 8)) & 1 if (not cs_n and idx < len(out)) else 1
            ivec = 0
            for n in range(6):
                v = pin(n)
                if n == self.pads["miso"] and not cs_n:
                    v = miso
                ivec |= v << n
            yield dut.pads_i.eq(ivec)
            if self.busy:
                self.busy -= 1
            clk_d, cs_d = clk, cs_n
            yield

    def response(self, cmd):
        # Bytes output after the bytes received so far (cmd), appended to the response stream.
        c = cmd[0]
        n = len(cmd)
        if c == 0x9f:
            return [JEDEC_ID[n - 1] if n <= 3 else 0] if n >= 1 else []
        if c == 0x05:
            return [0x01 if self.busy else 0x00 | (self.wel << 1)]
        if c == 0x03 and n >= 4:
            addr = (cmd[1] << 16) | (cmd[2] << 8) | cmd[3]
            return [self.mem[(addr + n - 4) % len(self.mem)]]
        return [0xff]

    def end(self, cmd):
        if not cmd:
            return
        c = cmd[0]
        if c == 0x06:
            self.wel = 1
        elif c == 0x02 and self.wel and len(cmd) > 4:
            addr = (cmd[1] << 16) | (cmd[2] << 8) | cmd[3]
            for i, b in enumerate(cmd[4:]):
                a = (addr & ~0xff) | ((addr + i) & 0xff)
                self.mem[a] &= b
            self.wel, self.busy = 0, 50
        elif c == 0x20 and self.wel and len(cmd) == 4:
            addr = ((cmd[1] << 16) | (cmd[2] << 8) | cmd[3]) & ~0xfff
            self.mem[addr:addr + 4096] = bytearray([0xff])*4096
            self.wel, self.busy = 0, 100

# Host Helpers -------------------------------------------------------------------------------------

def wb_write_bytes(dut, data):
    data = list(data) + [0]*((-len(data)) % 4)
    for i in range(0, len(data), 4):
        yield from dut.bus.write(i//4, int.from_bytes(bytes(data[i:i+4]), "little"))

def wb_read_bytes(dut, n):
    data = []
    for i in range((n + 3)//4):
        w = (yield from dut.bus.read(i))
        data += list(w.to_bytes(4, "little"))
    return data[:n]

def xfer(dut, data, hold=0):
    yield from wb_write_bytes(dut, data)
    yield dut.length.eq(len(data))
    yield dut.hold.eq(hold)
    yield dut.start.eq(1)
    yield
    yield dut.start.eq(0)
    yield
    while (yield dut.busy):
        yield
    return (yield from wb_read_bytes(dut, len(data)))

def set_pins(dut, mapping):
    for sig, role in zip(dut.pins, ["clk", "cs_n", "mosi", "miso"]):
        yield sig.eq(mapping[role])
    yield dut.enable.eq(1)
    yield

def wait_ready(dut):
    while True:
        r = yield from xfer(dut, [0x05, 0x00])
        if not (r[1] & 1):
            return

# Tests --------------------------------------------------------------------------------------------

HIDDEN = {"clk": 0, "cs_n": 2, "mosi": 1, "miso": 3} # 13th probed mapping (sim time).

class TestFlash(unittest.TestCase):
    def run_flash(self, scenario):
        dut   = Flash(npads=6)
        model = SPIFlashModel(HIDDEN)
        result = {}
        def host():
            yield dut.divider.eq(0)
            yield from scenario(dut, model, result)
        run_simulation(dut, [host(), model.generator(dut)])
        return result

    def test_disabled_inputs(self):
        # Disabled (default): no pad driven.
        def scenario(dut, model, result):
            yield
            result["oe"] = (yield dut.pads_oe)
        self.assertEqual(self.run_flash(scenario)["oe"], 0)

    def test_probe(self):
        # Host probe: JEDEC ID read with each (clk, cs_n, mosi, miso) mapping until a valid ID.
        def scenario(dut, model, result):
            for perm in itertools.permutations(range(6), 4):
                mapping = dict(zip(["clk", "cs_n", "mosi", "miso"], perm))
                yield from set_pins(dut, mapping)
                r = yield from xfer(dut, [0x9f, 0, 0, 0])
                if r[1:] == JEDEC_ID:
                    result["mapping"] = mapping
                    return
        self.assertEqual(self.run_flash(scenario)["mapping"], HIDDEN)

    def test_program_read(self):
        page = [(i*7 + 3) & 0xff for i in range(256)]
        def scenario(dut, model, result):
            yield from set_pins(dut, HIDDEN)
            # Sector erase, page program, read back.
            yield from xfer(dut, [0x06])
            yield from xfer(dut, [0x20, 0x01, 0x00, 0x00])
            yield from wait_ready(dut)
            yield from xfer(dut, [0x06])
            yield from xfer(dut, [0x02, 0x01, 0x00, 0x00] + page)
            yield from wait_ready(dut)
            r = yield from xfer(dut, [0x03, 0x01, 0x00, 0x00] + [0]*256)
            result["read"] = r[4:]
            # Multi-part read (hold): header then data in a second transfer.
            yield from xfer(dut, [0x03, 0x01, 0x00, 0x10], hold=1)
            r = yield from xfer(dut, [0]*16)
            result["read_hold"] = r
        result = self.run_flash(scenario)
        self.assertEqual(result["read"], page)
        self.assertEqual(result["read_hold"], page[16:32])
