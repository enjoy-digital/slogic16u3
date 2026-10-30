#!/usr/bin/env python3

#
# This file is part of SLogic16U3.
#
# Copyright (c) 2026 Florent Kermarrec <florent@enjoy-digital.fr>
# SPDX-License-Identifier: BSD-2-Clause

"""USB3 bring-up debug client (gateware --with-usb3-debug --with-jtagbone, through litex_server).

    litex_server --jtag --jtag-config software/debug/openocd_slogic16u3.cfg &
    ./software/debug/usb3_debug.py --csr-csv build/csr.csv status
    ./software/debug/usb3_debug.py --csr-csv build/csr.csv bulk-in
    ./software/debug/usb3_debug.py --csr-csv build/csr.csv history
    ./software/debug/usb3_debug.py --csr-csv build/csr.csv snapshot
    ./software/debug/usb3_debug.py --csr-csv build/csr.csv watch
    ./software/debug/usb3_debug.py --csr-csv build/csr.csv upar-read 0x808104
    ./software/debug/usb3_debug.py --csr-csv build/csr.csv upar-write 0x808104 0x000a2233
"""

import time
import argparse

from litex import RemoteClient

from usb3_pipe.ltssm import USB3LTSSM

GTR12_STATUS = [
    ("pll_lock",    0,  1), ("cdr_lock",   1,  1), ("aligned", 2, 1), ("k_lock", 3, 1),
    ("rx_elecidle", 4,  1), ("astat",      5,  6), ("stat",   11, 13),
    ("refclk_det", 24,  2), ("plls_lock", 26,  3),
]
TRANSITIONS   = ["rx_elecidle"] + [f"astat{i}" for i in range(6)] + [f"stat{i}" for i in range(13)]
PIPE_COUNTERS = ["com", "k28_4", "ts1", "ts1_inv", "ts2"]

# Helpers ------------------------------------------------------------------------------------------

class USB3Debug:
    def __init__(self, bus):
        self.bus    = bus
        # LTSSM states encoding of the gateware (USB3PHY with_u1u2=False).
        ltssm = USB3LTSSM(sys_clk_freq=62.5e6, with_u1u2=False)
        ltssm.finalize()
        self.states = {v: k for k, v in ltssm.fsm.encoding.items()}

    def reg(self, name):
        return getattr(self.bus.regs, name)

    def indexed(self, name, nbits):
        """Wide value read word by word (<name>_sel/<name>_data)."""
        value = 0
        for i in range((nbits + 31)//32):
            self.reg(f"usb3_debug_{name}_sel").write(i)
            value |= self.reg(f"usb3_debug_{name}_data").read() << (32*i)
        return value

    def fields(self, value, width, names):
        return {n: (value >> (width*i)) & (2**width - 1) for i, n in enumerate(names)}

    def phy_status(self):
        s = self.reg("usb3_phy_status").read()
        return {"ltssm_state": self.states.get(s & 0xff, f"?{s & 0xff}"), "u0": (s >> 8) & 1,
            "ready": (s >> 9) & 1}

    def gtr12_status(self):
        s = self.reg("usb3_serdes_gtr12_status").read()
        return {n: (s >> o) & (2**w - 1) for n, o, w in GTR12_STATUS}

    def transitions(self):
        return self.fields(self.indexed("gtr12_transitions", 16*len(TRANSITIONS)), 16, TRANSITIONS)

    def pipe_counters(self):
        return self.fields(self.indexed("pipe_counters", 16*len(PIPE_COUNTERS)), 16, PIPE_COUNTERS)

    def history(self, n=16):
        """LTSSM history, oldest first: (time_us, state)."""
        # Entries: Cat(state (LTSSM state width), time_us[24]), frozen while read.
        sw = max(self.states).bit_length()
        self.reg("usb3_debug_ltssm_history_freeze").write(1)
        v  = self.indexed("ltssm_history", 32*n)
        self.reg("usb3_debug_ltssm_history_freeze").write(0)
        h  = [((v >> (32*i + sw)) & 0xffffff, (v >> (32*i)) & (2**sw - 1)) for i in range(n)]
        return [(t, self.states.get(s, f"?{s}")) for t, s in reversed(h) if (t, s) != (0, 0)]

    def snapshot(self):
        self.reg("usb3_debug_rx_snapshot_arm").write(1)
        time.sleep(1.0) # Training attempts every ~200ms.
        raw   = self.indexed("rx_snapshot_raw", 8*(40 + 4*10))
        words = self.indexed("rx_snapshot_words", 16*37) if hasattr(self.bus.regs, "usb3_debug_rx_snapshot_words_sel") else None
        r = {"raw": [], "words": []}
        for i in range(8):
            e   = (raw >> (80*i)) & (2**80 - 1)
            dec = [(e >> (40 + 10*j)) & 0x3ff for j in range(4)]
            r["raw"].append((e & (2**40 - 1), [(d & 0xff, (d >> 8) & 1, (d >> 9) & 1) for d in dec]))
        for i in range(16 if words is not None else 0):
            e = (words >> (37*i)) & (2**37 - 1)
            r["words"].append((e & 0xffffffff, (e >> 32) & 0xf, (e >> 36) & 1))
        return r

    def rings(self, n=16):
        """Link Layer RX DPP / EP0 RX rings, oldest first."""
        self.reg("usb3_debug_ring_freeze").write(1)
        link = self.indexed("link_rx_dpp", 64*n)
        ep0  = self.indexed("ep0_rx", 64*n)
        self.reg("usb3_debug_ring_freeze").write(0)
        r = {"link": [], "ep0": []}
        for i in reversed(range(n)):
            e = (link >> (64*i)) & (2**64 - 1)
            r["link"].append(dict(data=e & 0xffffffff, ept=(e >> 32) & 0xf, first=(e >> 36) & 1,
                last=(e >> 37) & 1, setup=(e >> 38) & 1, valid=(e >> 39) & 1, done=(e >> 40) & 1,
                ok=(e >> 41) & 1))
            e = (ep0 >> (64*i)) & (2**64 - 1)
            r["ep0"].append(dict(data=e & 0xffffffff, first=(e >> 32) & 1, last=(e >> 33) & 1,
                setup=(e >> 34) & 1))
        return r

    BULK_IN_COUNTERS = ["cycles", "dps", "acks", "no_data", "no_win", "fc", "hp_wait", "all_sent",
        "credit", "phy_wait", "tx_idle"]

    def bulk_in(self):
        """Bulk IN throughput counters (last capture)."""
        n = len(self.BULK_IN_COUNTERS)
        v = self.indexed("bulk_in", 32*n + 16*17)
        c = {k: (v >> (32*i)) & 0xffffffff for i, k in enumerate(self.BULK_IN_COUNTERS)}
        c["nump"] = [(v >> (32*n + 16*i)) & 0xffff for i in range(17)]
        return c

    def upar_read(self, addr):
        self.reg("usb3_serdes_gtr12_upar_addr").write(addr)
        self.reg("usb3_serdes_gtr12_upar_ctrl").write(0b10)
        while not (self.reg("usb3_serdes_gtr12_upar_status").read() & 1):
            pass
        return self.reg("usb3_serdes_gtr12_upar_rddata").read()

    def upar_write(self, addr, data):
        self.reg("usb3_serdes_gtr12_upar_addr").write(addr)
        self.reg("usb3_serdes_gtr12_upar_wrdata").write(data)
        self.reg("usb3_serdes_gtr12_upar_ctrl").write(0b01)
        while not (self.reg("usb3_serdes_gtr12_upar_status").read() & 1):
            pass

# Commands -----------------------------------------------------------------------------------------

def print_status(d):
    print("PHY   :", d.phy_status())
    print("GTR12 :", {k: (hex(v) if k in ["astat", "stat"] else v) for k, v in d.gtr12_status().items()})
    if hasattr(d.bus.regs, "usb3_debug_gtr12_transitions_sel"):
        print("Trans :", {k: v for k, v in d.transitions().items() if v})
        print("PIPE  :", d.pipe_counters())
    if hasattr(d.bus.regs, "usb3_core_status"):
        print("Core  :", {n: hex(d.reg(f"usb3_core_{n}").read()) for n in ["status", "rx_lbad", "tx_retry",
            "recovery_req", "recovery_cause"]})

def main():
    parser = argparse.ArgumentParser(description="SLogic16U3 USB3 bring-up debug (JTAGBone).")
    parser.add_argument("--csr-csv", default="build/csr.csv", help="CSR configuration file.")
    parser.add_argument("--port",    default=1234, type=int,  help="litex_server port.")
    parser.add_argument("command",   choices=["status", "bulk-in", "history", "snapshot", "rings", "watch", "upar-read", "upar-write"])
    parser.add_argument("args",      nargs="*")
    args = parser.parse_args()

    bus = RemoteClient(csr_csv=args.csr_csv, port=args.port)
    bus.open()
    d = USB3Debug(bus)
    try:
        if args.command == "status":
            print_status(d)
        elif args.command == "bulk-in":
            c      = d.bulk_in()
            cycles = max(c["cycles"], 1)
            rate   = c["dps"]*1024/(cycles/62.5e6)/1e6
            print(f"{c['cycles']} cycles ({c['cycles']/62.5e6*1e3:.2f}ms), {c['dps']} DPs: "
                f"{rate:.1f}MB/s, {cycles/max(c['dps'], 1):.1f} cycles/DP")
            print("  ACK NumP: " + " ".join(f"{i}:{v}" for i, v in enumerate(c.pop("nump")) if v))
            for k, v in c.items():
                if k != "cycles":
                    print(f"  {k:10s} {v:10d} {100*v/cycles:5.1f}%")
        elif args.command == "history":
            for t, s in d.history():
                print(f"{t:10d}us {s}")
        elif args.command == "snapshot":
            r = d.snapshot()
            for raw, dec in r["raw"]:
                print(f"raw {raw:010x} " + " ".join(f"{'K' if k else 'D'}{v:02x}{'!' if e else ''}" for v, k, e in dec))
            for data, ctrl, valid in r["words"]:
                print(f"word {data:08x} ctrl {ctrl:x} valid {valid}")
        elif args.command == "rings":
            r = d.rings()
            print("Link Layer RX DPP:")
            for e in r["link"]:
                print(f"  {e['data']:08x} ept {e['ept']} " + " ".join(k for k in ["first", "last", "setup", "valid", "done", "ok"] if e[k]))
            print("EP0 RX:")
            for e in r["ep0"]:
                print(f"  {e['data']:08x} " + " ".join(k for k in ["first", "last", "setup"] if e[k]))
        elif args.command == "watch":
            last = None
            while True:
                s = (d.phy_status(), d.gtr12_status())
                if s != last:
                    print(time.strftime("%H:%M:%S"), s[0], {k: v for k, v in s[1].items() if k not in ["stat"]})
                    last = s
                time.sleep(0.05)
        elif args.command == "upar-read":
            for a in args.args:
                print(f"{int(a, 0):#08x}: {d.upar_read(int(a, 0)):#010x}")
        elif args.command == "upar-write":
            d.upar_write(int(args.args[0], 0), int(args.args[1], 0))
    except KeyboardInterrupt:
        pass
    finally:
        bus.close()

if __name__ == "__main__":
    main()
