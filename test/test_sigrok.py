#
# This file is part of SLogic16U3.
#
# Copyright (c) 2026 Florent Kermarrec <florent@enjoy-digital.fr>
# SPDX-License-Identifier: BSD-2-Clause

"""sigrok: sigrok-cli with the litex-slogic driver on the simulated device (counter pattern data checked
through the whole libsigrok path, pattern generator decoded by libsigrokdecode). Skipped when
software/sigrok/build.sh has not been run."""

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

    def check_trigger(self, channels, samplerate, samples, ratio, trigger, t, extra=()):
        # Counter pattern (sample i = i*divider): trigger at sample t, capture ratio % of pre-trigger
        # samples: the output starts at t - pre (stream window of the hardware trigger, trimmed).
        divider = 800_000_000//samplerate
        chans   = ",".join(f"CH{i}" for i in range(channels))
        pre     = min(t, ratio*samples//100)
        with tempfile.TemporaryDirectory() as d:
            out = os.path.join(d, "data.bin")
            r   = self.run_cli("-l", "4", "-C", chans, "-c",
                f"samplerate={samplerate}:pattern=counter:captureratio={ratio}",
                "--samples", str(samples), "--triggers", trigger, "--wait-trigger", *extra,
                "-O", "binary", "-o", out)
            self.assertEqual(r.returncode, 0, r.stderr)
            data = open(out, "rb").read()
        log = r.stderr.decode()
        unit = 2 if channels > 8 else 1
        mask = (1 << channels) - 1
        got  = [int.from_bytes(data[unit*i:unit*(i + 1)], "little") & mask for i in range(len(data)//unit)]
        first = t - pre
        ref   = [((first + i)*divider) & 0xffff & mask for i in range(len(got))]
        bad   = next((i for i in range(len(got)) if got[i] != ref[i]), None)
        self.assertIsNone(bad, f"sample {bad}")
        return log, len(got), pre

    def test_trigger_hw(self):
        # 16 channels at 200MS/s: CH14 high, others low (0x4000 = 4*4096): sample 4096, 10% pre-trigger.
        cond = ",".join(f"CH{i}={1 if i == 14 else 0}" for i in range(16))
        log, n, pre = self.check_trigger(16, 200_000_000, 20000, 10, cond, 4096)
        self.assertIn("Hardware trigger", log)
        self.assertIn("SR_DF_TRIGGER", log)
        self.assertEqual(n, 20000)
        # 4 channels at 800MS/s: CH3 rising (sample 8), early trigger: 8 pre-trigger samples only.
        log, n, pre = self.check_trigger(4, 800_000_000, 1000, 50, "CH3=r", 8)
        self.assertIn("Hardware trigger", log)
        self.assertEqual(n, 8 + 500)

    def test_trigger_soft_fallback(self):
        # Pre-trigger deeper than the hardware buffer: soft trigger (CH7 rising: sample 16, 8*16 = 128).
        log, n, pre = self.check_trigger(8, 100_000_000, 200000, 50, "CH7=r", 16)
        self.assertIn("soft trigger", log)
        self.assertGreater(n, 0)

    def test_uart_decode(self):
        # Simulated inputs: UART 1Mbaud on CH0, decoded by libsigrokdecode.
        r = self.run_cli("-c", "samplerate=10M", "--samples", "2000000", "-P", "uart:rx=CH0:baudrate=1000000",
            "-A", "uart=rx-data", "--protocol-decoder-ann-class")
        if b"protocol decoder" in r.stderr.lower() and r.returncode != 0:
            self.skipTest("sigrok-cli without libsigrokdecode")
        self.assertEqual(r.returncode, 0, r.stderr)
        text = "".join(chr(int(l.split()[-1], 16)) for l in r.stdout.decode().splitlines() if l.strip())
        self.assertIn("Hello from SLogic16U3!", text)

    def decode(self, samplerate, samples, decoder, ann, channels=None):
        args = ["-c", f"samplerate={samplerate}:pattern=generator", "--samples", str(samples), "-P", decoder,
            "-A", ann]
        r = self.run_cli(*(["-C", channels] if channels else []), *args)
        if b"protocol decoder" in r.stderr.lower() and r.returncode != 0:
            self.skipTest("sigrok-cli without libsigrokdecode")
        self.assertEqual(r.returncode, 0, r.stderr)
        return [int(l.split()[-1], 16) for l in r.stdout.decode().splitlines() if l.strip()]

    def test_generator_uart(self):
        # Pattern generator: UART 1Mbaud on CH0, decoded from the first character.
        data = self.decode("100M", 200000, "uart:rx=CH0:baudrate=1000000", "uart=rx-data")
        text = "SLogic16U3 LiteX pattern generator\r\n"
        self.assertEqual("".join(map(chr, data))[:2*len(text)], 2*text)

    def test_generator_spi(self):
        # Pattern generator: SPI mode 0 on CH1-CH3 ("SLOG" frames), 4 channels at 800MS/s.
        data = self.decode("800M", 20000, "spi:cs=CH1:clk=CH2:mosi=CH3", "spi=mosi-data", channels="CH0,CH1,CH2,CH3")
        self.assertGreater(len(data), 8)
        self.assertEqual(data[:8], [0x53, 0x4c, 0x4f, 0x47]*2)

    def test_invalid_config(self):
        r = self.run_cli("-c", "samplerate=400M", "--samples", "10")
        self.assertNotEqual(r.returncode, 0)
        self.assertIn(b"too high for 16 channels", r.stderr)
