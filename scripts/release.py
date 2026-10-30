#!/usr/bin/env python3

#
# This file is part of SLogic16U3.
#
# Copyright (c) 2026 Enjoy-Digital <enjoy-digital.fr>
# SPDX-License-Identifier: BSD-2-Clause

"""Release package: gateware (Flash .bin for the DFU loader/remote update, SRAM .fs for JTAG), Linux tools
(slogic16u3_util, slogic16u3_record), udev rules, install guide (doc/INSTALL.md), build information and checksums.

    ./scripts/build_best.py                                   # Bitstream in build/ (timings met).
    ./scripts/release.py 0.1.0 [--build-dir build] [--allow-dirty]
    # -> release/slogic16u3-0.1.0/ and release/slogic16u3-0.1.0.tar.gz

Refused: a build without its timings met, a bitstream that is not a GW5AT-15 image, uncommitted changes
(unless --allow-dirty).
"""

import os
import sys
import shutil
import hashlib
import tarfile
import argparse
import datetime
import subprocess

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(ROOT, "scripts"))

from build_best import fmax_summary, worst_ratio

GW5AT15_IDCODE = 0x0001681b

def git(path, *args):
    r = subprocess.run(["git", "-C", path, *args], capture_output=True, text=True)
    return r.stdout.strip() if r.returncode == 0 else None

def image_check(data):
    """GW5AT-15 bitstream (as libslogic16u3 slogic_image_check): sync word a5c3, IDCODE command 0x06."""
    i = data.find(b"\xa5\xc3", 0, 1024)
    if i < 0 or data[i + 2] != 0x06:
        return "no Gowin sync word/IDCODE in the header"
    idcode = int.from_bytes(data[i + 6:i + 10], "big")
    if idcode != GW5AT15_IDCODE:
        return f"IDCODE 0x{idcode:08x}, not the GW5AT-15"
    if len(data) > 0x800000:
        return "larger than the application region (8MB)"
    return None

def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("version",                                                 help="Release version (ex: 0.1.0).")
    parser.add_argument("--build-dir",   default=os.path.join(ROOT, "build"),     help="Gateware build directory.")
    parser.add_argument("--output-dir",  default=os.path.join(ROOT, "release"),   help="Output directory.")
    parser.add_argument("--allow-dirty", action="store_true",                      help="Allow uncommitted changes.")
    args = parser.parse_args()

    name = f"slogic16u3-{args.version}"
    out  = os.path.join(args.output_dir, name)

    # Checks.
    if git(ROOT, "status", "--porcelain") and not args.allow_dirty:
        sys.exit("Uncommitted changes (--allow-dirty to package anyway).")
    gw  = os.path.join(args.build_dir, "gateware")
    binf, fsf = os.path.join(gw, "sipeed_slogic16u3.bin"), os.path.join(gw, "sipeed_slogic16u3.fs")
    for f in [binf, fsf]:
        if not os.path.exists(f):
            sys.exit(f"{f} missing: build the gateware first (scripts/build_best.py).")
    timings = fmax_summary(args.build_dir)
    if not timings or worst_ratio(timings) < 1.0:
        sys.exit(f"Timings not met in {args.build_dir}: {timings}")
    data = open(binf, "rb").read()
    err  = image_check(data)
    if err:
        sys.exit(f"{binf}: {err}")

    # Tools.
    subprocess.run(["make", "-s", "-C", os.path.join(ROOT, "software", "user")], check=True)

    # Package.
    if os.path.exists(out):
        shutil.rmtree(out)
    files = {
        f"gateware/{name}.bin"                  : binf,
        f"gateware/{name}.fs"                   : fsf,
        "gateware/csr.csv"                      : os.path.join(args.build_dir, "csr.csv"),
        "tools/linux-x86_64/slogic16u3_util"        : os.path.join(ROOT, "software", "user", "build", "slogic16u3_util"),
        "tools/linux-x86_64/slogic16u3_record"      : os.path.join(ROOT, "software", "user", "build", "slogic16u3_record"),
        "udev/99-slogic16u3.rules"              : os.path.join(ROOT, "software", "udev", "99-slogic16u3.rules"),
        "INSTALL.md"                            : os.path.join(ROOT, "doc", "INSTALL.md"),
        "LICENSE"                               : os.path.join(ROOT, "LICENSE"),
    }
    for dst, src in files.items():
        if not os.path.exists(src):
            sys.exit(f"{src} missing.")
        os.makedirs(os.path.dirname(os.path.join(out, dst)), exist_ok=True)
        shutil.copy2(src, os.path.join(out, dst))

    # Build information.
    deps = [("slogic_16u3", ROOT)]
    for p in os.environ.get("PYTHONPATH", "").split(":") + [os.path.join(os.environ.get("LITEX_DIR",
        os.path.expanduser("~/dev/litex")), r) for r in ["litex", "migen", "litex-boards", "liteiclink", "usb3_pipe"]]:
        if p and os.path.isdir(p) and git(p, "rev-parse", "HEAD"):
            top = git(p, "rev-parse", "--show-toplevel") or p
            deps.append((os.path.basename(top.split("/.claude/")[0]), p))
    gowin = open(os.path.join(gw, "impl", "pnr", "project.rpt.txt"), errors="ignore").read()
    gowin = next((l.split(":", 1)[1].strip() for l in gowin.splitlines() if "<Tool Version>" in l), "?")
    with open(os.path.join(out, "BUILDINFO.txt"), "w") as f:
        f.write(f"SLogic16U3 LiteX gateware/tools {args.version}\n")
        f.write(f"Date      : {datetime.datetime.now(datetime.timezone.utc):%Y-%m-%d %H:%M UTC}\n")
        f.write(f"Toolchain : Gowin {gowin}, GW5AT-LV15MG132C1/I0 (version B)\n")
        f.write("Timings   : " + ", ".join(f"{k} {v[1]:.1f}/{v[0]:.1f}MHz" for k, v in timings.items()) + "\n")
        f.write("Sources   :\n")
        for n, p in deps:
            rev, branch = git(p, "rev-parse", "--short=10", "HEAD"), git(p, "rev-parse", "--abbrev-ref", "HEAD")
            f.write(f"  {n:13s} {rev} ({branch})"
                    f"{' dirty' if git(p, 'status', '--porcelain', '--untracked-files=no') else ''}\n")

    # Checksums, archive.
    sums = []
    for d, _, fs in sorted(os.walk(out)):
        for fn in sorted(fs):
            p = os.path.join(d, fn)
            sums.append(f"{hashlib.sha256(open(p, 'rb').read()).hexdigest()}  {os.path.relpath(p, out)}")
    open(os.path.join(out, "SHA256SUMS"), "w").write("\n".join(sums) + "\n")
    tgz = out + ".tar.gz"
    with tarfile.open(tgz, "w:gz") as t:
        t.add(out, arcname=name)
    print(open(os.path.join(out, "BUILDINFO.txt")).read())
    print("\n".join(sums))
    print(f"\n{tgz}: {os.path.getsize(tgz)} bytes, sha256 {hashlib.sha256(open(tgz, 'rb').read()).hexdigest()}")

if __name__ == "__main__":
    main()
