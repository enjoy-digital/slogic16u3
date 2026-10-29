#
# This file is part of SLogic16U3.
#
# Copyright (c) 2026 Florent Kermarrec <florent@enjoy-digital.fr>
# SPDX-License-Identifier: BSD-2-Clause

"""sigrok: sigrok-cli with the litex-slogic driver on the simulated device (counter pattern data checked
through the whole libsigrok path). Skipped when software/sigrok/build.sh has not been run."""

import os
import subprocess
import tempfile
import unittest

ROOT    = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
INSTALL = os.path.join(ROOT, "software", "sigrok", "install")
CLI     = os.path.join(INSTALL, "bin", "sigrok-cli")

@unittest.skipUnless(os.path.exists(CLI), "sigrok-cli not built (software/sigrok/build.sh)")
class TestSigrok(unittest.TestCase):
    def run_cli(self, *args):
        env = dict(os.environ, LD_LIBRARY_PATH=os.path.join(INSTALL, "lib"))
        return subprocess.run([CLI, "-d", "litex-slogic:conn=sim,fast", *args], env=env,
            capture_output=True, timeout=120)

    def test_scan(self):
        r = self.run_cli("--scan")
        self.assertIn(b"SLogic16U3", r.stdout)
        self.assertIn(b"16 channels", r.stdout)

    def check_counter(self, channels, samplerate, samples):
        divider = 800_000_000//samplerate
        chans   = ",".join(f"CH{i}" for i in range(channels))
        with tempfile.TemporaryDirectory() as d:
            out = os.path.join(d, "data.bin")
            r   = self.run_cli("-C", chans, "-c", f"samplerate={samplerate}:pattern=counter",
                "--samples", str(samples), "-O", "binary", "-o", out)
            self.assertEqual(r.returncode, 0, r.stderr)
            data = open(out, "rb").read()
        unit = 2 if channels > 8 else 1
        self.assertEqual(len(data), samples*unit)
        mask = (1 << channels) - 1
        for i in range(samples):
            v = int.from_bytes(data[unit*i:unit*(i + 1)], "little") & mask
            self.assertEqual(v, (i*divider) & 0xffff & mask, f"sample {i}")

    def test_counter_16ch(self):
        self.check_counter(16, 200_000_000, 50000)

    def test_counter_8ch(self):
        self.check_counter(8, 400_000_000, 50001)

    def test_counter_4ch(self):
        self.check_counter(4, 800_000_000, 9999)

    def test_uart_decode(self):
        # Simulated inputs: UART 1Mbaud on CH0, decoded by libsigrokdecode.
        r = self.run_cli("-c", "samplerate=10M", "--samples", "2000000", "-P", "uart:rx=CH0:baudrate=1000000",
            "-A", "uart=rx-data", "--protocol-decoder-ann-class")
        if b"protocol decoder" in r.stderr.lower() and r.returncode != 0:
            self.skipTest("sigrok-cli without libsigrokdecode")
        self.assertEqual(r.returncode, 0, r.stderr)
        text = "".join(chr(int(l.split()[-1], 16)) for l in r.stdout.decode().splitlines() if l.strip())
        self.assertIn("Hello from SLogic16U3!", text)

    def test_invalid_config(self):
        r = self.run_cli("-c", "samplerate=400M", "--samples", "10")
        self.assertNotEqual(r.returncode, 0)
        self.assertIn(b"too high for 16 channels", r.stderr)
