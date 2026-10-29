#
# This file is part of SLogic16U3.
#
# Copyright (c) 2026 Florent Kermarrec <florent@enjoy-digital.fr>
# SPDX-License-Identifier: BSD-2-Clause

"""ngscopeclient bridge: slogic_scopehal_bridge on the simulated device, driven as libscopehal does
(SCPI control port, waveform port): identification, rates/depths, free-running and triggered waveforms
(counter pattern bits checked, trigger position). Skipped when the host software is not built."""

import os
import time
import socket
import struct
import subprocess
import unittest

ROOT   = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
BRIDGE = os.path.join(ROOT, "software", "user", "build", "slogic_scopehal_bridge")

FS = 1e15

@unittest.skipUnless(os.path.exists(BRIDGE), "bridge not built (make -C software/user)")
class TestScopehalBridge(unittest.TestCase):
    scpi_port = 15025
    wfm_port  = 15026

    def setUp(self):
        self.proc = subprocess.Popen([BRIDGE, "-d", "sim:fast", "--scpi-port", str(self.scpi_port),
            "--waveform-port", str(self.wfm_port)], stderr=subprocess.PIPE)
        for _ in range(100):
            try:
                self.scpi = socket.create_connection(("localhost", self.scpi_port))
                self.wfm  = socket.create_connection(("localhost", self.wfm_port))
                break
            except OSError:
                time.sleep(0.05)
        self.scpi.settimeout(10)
        self.wfm.settimeout(30)
        self.rx = b""

    def tearDown(self):
        self.scpi.close()
        self.wfm.close()
        self.proc.terminate()
        self.proc.wait(timeout=5)

    def cmd(self, line):
        self.scpi.sendall(line.encode() + b"\n")

    def query(self, line):
        self.cmd(line)
        while b"\n" not in self.rx:
            self.rx += self.scpi.recv(4096)
        reply, self.rx = self.rx.split(b"\n", 1)
        return reply.decode()

    def recv(self, n):
        data = b""
        while len(data) < n:
            chunk = self.wfm.recv(n - len(data))
            self.assertTrue(chunk)
            data += chunk
        return data

    def waveform(self):
        self.wfm.sendall(b"K")
        seq, nch, fs, trig, wfms = struct.unpack("<IHqqd", self.recv(4 + 2 + 8 + 8 + 8))
        channels = {}
        for _ in range(nch):
            ch, nbytes, first = struct.unpack("<QQi", self.recv(20))
            bits = self.recv(nbytes)
            channels[ch] = [(bits[i//8] >> (i % 8)) & 1 for i in range(8*nbytes)]
        return seq, fs, trig, channels

    def test_identification(self):
        idn = self.query("*IDN?").split(",")
        self.assertEqual(idn[0], "Sipeed")
        self.assertIn("SLogic16U3", idn[1])
        self.assertEqual(self.query("CHANS?"), "0")
        rates = [FS/float(x) for x in self.query("RATES?").split(",") if x]
        self.assertIn(800e6, [round(r) for r in rates])
        self.assertIn(1e6,   [round(r) for r in rates])
        depths = [int(x) for x in self.query("DEPTHS?").split(",") if x]
        self.assertIn(100000, depths)

    def test_free_running(self):
        # Counter pattern on CH0-CH7 at 100MHz (divider 8: sample i = 8*i).
        self.cmd("RATE 100000000")
        self.cmd("DEPTH 1000")
        for ch in range(8, 16):
            self.cmd(f":{ch}:OFF")
        self.cmd("START")
        self.assertEqual(self.query("ARMED?"), "1")
        seq, fs, trig, channels = self.waveform()
        self.assertEqual(fs, 10_000_000)
        self.assertEqual(sorted(channels), list(range(8)))
        for ch in range(8):
            bits = channels[ch][:1000]
            # Inputs (simulated): CHn = bit n of (8*i) >> 2.
            self.assertEqual(bits, [((8*i >> 2) >> ch) & 1 for i in range(1000)])
        seq2, *_ = self.waveform()
        self.assertEqual(seq2, seq + 1)

    def test_triggered_single(self):
        # Rising edge on CH3 (period 2^(3+2)*2 base samples at 800MHz/D), 100 pre-trigger samples.
        self.cmd("RATE 400000000")
        self.cmd("DEPTH 4096")
        for ch in range(8, 16):
            self.cmd(f":{ch}:OFF")
        self.cmd("TRIG:SOU 3")
        self.cmd("TRIG:EDGE:DIR RISING")
        self.cmd(f"TRIG:DELAY {int(100*FS/400e6)}")
        self.cmd("SINGLE")
        seq, fs, trig, channels = self.waveform()
        pre = trig//fs
        self.assertEqual(pre, 100)
        ch3 = channels[3]
        self.assertEqual(ch3[pre - 1], 0)
        self.assertEqual(ch3[pre], 1)
        # Single: disarmed after the waveform.
        self.assertEqual(self.query("ARMED?"), "0")

    def test_rate_fallback(self):
        # 400MHz with the 16 channels enabled: 200MHz waveforms (header sample period).
        self.cmd("RATE 400000000")
        self.cmd("DEPTH 1000")
        self.cmd("START")
        seq, fs, trig, channels = self.waveform()
        self.assertEqual(fs, 5_000_000)
        self.assertEqual(len(channels), 16)
