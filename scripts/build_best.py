#!/usr/bin/env python3

#
# This file is part of SLogic16U3.
#
# Copyright (c) 2026 Florent Kermarrec <florent@enjoy-digital.fr>
# SPDX-License-Identifier: BSD-2-Clause

"""Build the gateware with several Gowin place algorithms in parallel (Gowin PnR has no seed, the design
fills ~90% of the GW5AT-15: timings depend on the placement) and keep the one with the best margins (worst
fmax/constraint ratio) in build/.

    ./scripts/build_best.py [--options 0 1 2 3 4] [-- extra slogic_16u3.py arguments]
"""

import os
import re
import sys
import html
import shutil
import argparse
import subprocess

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))

def fmax_summary(build_dir):
    """{clock: (constraint MHz, fmax MHz)} from the Gowin timing report."""
    report = os.path.join(build_dir, "gateware", "impl", "pnr", "project_tr_content.html")
    if not os.path.exists(report):
        return None
    t   = open(report).read()
    seg = t[t.find("Max Frequency Summary"):][:4000]
    res = {}
    for row in re.findall(r"<tr>(.*?)</tr>", seg, re.S)[1:]:
        c = [html.unescape(re.sub("<.*?>", "", x)).strip() for x in re.findall(r"<t[dh][^>]*>(.*?)</t[dh]>", row, re.S)]
        if len(c) >= 4 and "MHz" in c[2]:
            res[c[1]] = (float(c[2].split("(")[0]), float(c[3].split("(")[0]))
    return res

def worst_ratio(summary):
    return min(fmax/target for target, fmax in summary.values())

def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--options",    nargs="+", type=int, default=[0, 1, 2, 3, 4], help="Gowin place algorithms.")
    parser.add_argument("--output-dir", default=os.path.join(ROOT, "build"),       help="Build directory.")
    parser.add_argument("extra",        nargs=argparse.REMAINDER,                  help="slogic_16u3.py arguments.")
    args  = parser.parse_args()
    extra = [a for a in args.extra if a != "--"]

    os.makedirs(os.path.dirname(os.path.abspath(args.output_dir)), exist_ok=True)
    procs = {}
    for opt in args.options:
        d = f"{args.output_dir}_place{opt}"
        log = open(f"{d}.log", "w")
        procs[opt] = (d, subprocess.Popen([sys.executable, os.path.join(ROOT, "slogic_16u3.py"), "--build",
            "--output-dir", d, "--place-option", str(opt)] + extra, stdout=log, stderr=subprocess.STDOUT, cwd=ROOT))
    results = {}
    for opt, (d, p) in procs.items():
        p.wait()
        s = fmax_summary(d) if p.returncode == 0 else None
        results[opt] = (d, s)
        state = "failed" if s is None else ", ".join(f"{k} {v[1]:.1f}/{v[0]:.1f}MHz" for k, v in s.items())
        print(f"place_option {opt}: {state}")

    ok = {o: r for o, r in results.items() if r[1] is not None}
    if not ok:
        print("No successful build.")
        return 1
    best = max(ok, key=lambda o: worst_ratio(ok[o][1]))
    met  = worst_ratio(ok[best][1]) >= 1.0
    print(f"Best: place_option {best} ({'timings met' if met else 'TIMINGS NOT MET'}) -> {args.output_dir}")
    if os.path.exists(args.output_dir):
        shutil.rmtree(args.output_dir)
    shutil.copytree(ok[best][0], args.output_dir)
    return 0 if met else 2

if __name__ == "__main__":
    sys.exit(main())
