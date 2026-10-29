#
# This file is part of SLogic16U3.
#
# Copyright (c) 2026 Florent Kermarrec <florent@enjoy-digital.fr>
# SPDX-License-Identifier: BSD-2-Clause

"""Register map: software/user/libslogic/slogic_regs.h against the csr.csv of the hardware target
(elaborated, not built): the fixed CSR map used by the host software."""

import os
import re
import tempfile
import unittest

from litex.soc.integration.builder import Builder

from litex_boards.platforms import sipeed_slogic16u3

from slogic_16u3 import SLogicSoC

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))

def header_defines():
    """#define NAME value (resolved, simple (BASE + 0xNN) expressions)."""
    text    = open(os.path.join(ROOT, "software", "user", "libslogic", "slogic_regs.h")).read()
    defines = {}
    for name, value in re.findall(r"#define\s+(SLOGIC_\w+)\s+([^/\n]+)", text):
        value = value.strip()
        m = re.match(r"^\((SLOGIC_\w+)\s*\+\s*(0x[0-9a-fA-F]+)\)$", value)
        if m and m.group(1) in defines:
            defines[name] = defines[m.group(1)] + int(m.group(2), 16)
        elif re.match(r"^(0x[0-9a-fA-F]+|\d+)$", value):
            defines[name] = int(value, 0)
    return defines

class TestRegs(unittest.TestCase):
    def test_csr_map(self):
        platform = sipeed_slogic16u3.Platform(toolchain="gowin")
        soc      = SLogicSoC(platform)
        with tempfile.TemporaryDirectory() as d:
            builder = Builder(soc, output_dir=d, csr_csv=os.path.join(d, "csr.csv"))
            builder.build(run=False)
            csv = open(os.path.join(d, "csr.csv")).read()
        regs, bases, mems = {}, {}, {}
        for line in csv.splitlines():
            f = line.split(",")
            if f[0] == "csr_register":
                regs[f[1]] = (int(f[2], 16), int(f[3]))
            elif f[0] == "csr_base":
                bases[f[1]] = int(f[2], 16)
            elif f[0] == "memory_region":
                mems[f[1]] = (int(f[2], 16), int(f[3]))
        h = header_defines()

        # Banks.
        for bank in ["ctrl", "analyzer", "flash", "usb3_phy", "identifier"]:
            name = "identifier_mem" if bank == "identifier" else bank
            self.assertEqual(h[f"SLOGIC_{bank.upper()}_BASE"], bases[name], bank)

        # Registers (header name -> csr.csv name, 32-bit words).
        checked = 0
        for name, value in h.items():
            m = re.match(r"^SLOGIC_(CTRL|ANALYZER|FLASH|USB3_PHY)_(\w+)$", name)
            if not m or m.group(2) == "BASE":
                continue
            csr = f"{m.group(1).lower()}_{m.group(2).lower()}"
            if csr not in regs:
                continue
            self.assertEqual(value, regs[csr][0], csr)
            checked += 1
        self.assertGreaterEqual(checked, 18)

        # 64-bit registers.
        for csr in ["analyzer_limit", "analyzer_count"]:
            self.assertEqual(regs[csr][1], 2, csr)

        # Flash buffer.
        self.assertEqual(mems["flash_buffer"], (h["SLOGIC_FLASH_BUFFER_BASE"], h["SLOGIC_FLASH_BUFFER_SIZE"]))
