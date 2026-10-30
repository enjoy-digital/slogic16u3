#!/bin/sh
#
# This file is part of SLogic16U3.
#
# Copyright (c) 2026 Enjoy-Digital <enjoy-digital.fr>
# SPDX-License-Identifier: BSD-2-Clause
#
# libsigrok with the litex-slogic driver, libsigrokdecode, sigrok-cli (and PulseView with --pulseview), built from source
# into a local prefix (the distribution packages are not touched):
#
#   ./build.sh [--pulseview] [--prefix DIR]     (default prefix: ./install)
#   ./install/bin/sigrok-cli -d litex-slogic --samples 1M -c samplerate=100M -O vcd -o capture.vcd
#   ./install/bin/sigrok-cli -d litex-slogic:conn=sim --scan
#   LD_LIBRARY_PATH=./install/lib ./install/bin/pulseview
#
# Also writes litex-slogic.patch (libsigrok driver, for upstream).

set -e
HERE=$(cd "$(dirname "$0")" && pwd)
PREFIX=$HERE/install
SRC=$HERE/build
PULSEVIEW=0
JOBS=$(nproc 2>/dev/null || echo 4)

LIBSIGROK_REPO=https://github.com/sigrokproject/libsigrok
LIBSIGROK_REV=0bc24877
SIGROK_CLI_REPO=https://github.com/sigrokproject/sigrok-cli
SIGROK_CLI_REV=f44dd91
LIBSIGROKDECODE_REPO=https://github.com/sigrokproject/libsigrokdecode
LIBSIGROKDECODE_REV=master
PULSEVIEW_REPO=https://github.com/sigrokproject/pulseview
PULSEVIEW_REV=master

while [ $# -gt 0 ]; do
    case $1 in
        --pulseview) PULSEVIEW=1 ;;
        --prefix)    PREFIX=$2; shift ;;
        *)           echo "unknown option $1"; exit 1 ;;
    esac
    shift
done
mkdir -p "$SRC" "$PREFIX"
export PKG_CONFIG_PATH=$PREFIX/lib/pkgconfig${PKG_CONFIG_PATH:+:$PKG_CONFIG_PATH}

fetch() { # fetch <dir> <repo> <rev>
    [ -d "$SRC/$1/.git" ] || git clone -q "$2" "$SRC/$1"
    git -C "$SRC/$1" fetch -q origin
    git -C "$SRC/$1" checkout -q -f "$3"
}

# libsigrok + litex-slogic driver ------------------------------------------------------------------
fetch libsigrok $LIBSIGROK_REPO $LIBSIGROK_REV
D=$SRC/libsigrok/src/hardware/litex-slogic
rm -rf "$D" && mkdir -p "$D/libslogic16u3"
cp "$HERE"/litex-slogic/*.[ch] "$D/"
for f in slogic.h slogic_internal.h slogic_regs.h slogic.c slogic_usb.c slogic_sim.c; do
    cp "$HERE/../user/libslogic16u3/$f" "$D/libslogic16u3/"
done
cd "$SRC/libsigrok"
if ! grep -q "litex-slogic" configure.ac; then
    sed -i 's/^SR_DRIVER(\[LeCroy LogicStudio\]/SR_DRIVER([Sipeed SLogic16U3 (LiteX)], [litex-slogic], [libusb])\n&/' configure.ac
    grep -q "litex-slogic" configure.ac || sed -i 's/^SR_DRIVER(\[demo\], \[demo\])/&\nSR_DRIVER([Sipeed SLogic16U3 (LiteX)], [litex-slogic], [libusb])/' configure.ac
    python3 - <<'EOF'
import re
t = open("Makefile.am").read()
block = """if HW_LITEX_SLOGIC
src_libdrivers_la_SOURCES += \\
	src/hardware/litex-slogic/protocol.h \\
	src/hardware/litex-slogic/protocol.c \\
	src/hardware/litex-slogic/api.c \\
	src/hardware/litex-slogic/libslogic16u3/slogic.h \\
	src/hardware/litex-slogic/libslogic16u3/slogic_internal.h \\
	src/hardware/litex-slogic/libslogic16u3/slogic_regs.h \\
	src/hardware/litex-slogic/libslogic16u3/slogic.c \\
	src/hardware/litex-slogic/libslogic16u3/slogic_usb.c \\
	src/hardware/litex-slogic/libslogic16u3/slogic_sim.c
endif
"""
t = t.replace("if HW_DEMO\n", block + "if HW_DEMO\n", 1)
open("Makefile.am", "w").write(t)
EOF
fi
git add -A src/hardware/litex-slogic >/dev/null 2>&1 || true
git diff HEAD > "$HERE/litex-slogic.patch"
[ -f configure ] || ./autogen.sh
[ -f Makefile ] || ./configure -q --prefix="$PREFIX" --disable-all-drivers --enable-litex-slogic \
    --enable-demo --enable-fx2lafw --disable-java --disable-python --disable-ruby
make -s -j"$JOBS" && make -s install

# libsigrokdecode (protocol decoders) --------------------------------------------------------------
fetch libsigrokdecode $LIBSIGROKDECODE_REPO $LIBSIGROKDECODE_REV
cd "$SRC/libsigrokdecode"
[ -f configure ] || ./autogen.sh
[ -f Makefile ] || ./configure -q --prefix="$PREFIX"
make -s -j"$JOBS" && make -s install

# sigrok-cli ---------------------------------------------------------------------------------------
fetch sigrok-cli $SIGROK_CLI_REPO $SIGROK_CLI_REV
cd "$SRC/sigrok-cli"
[ -f configure ] || ./autogen.sh
./configure -q --prefix="$PREFIX" # Always: libsigrokdecode detected.
make -s -j"$JOBS" && make -s install

# PulseView (optional) -----------------------------------------------------------------------------
if [ $PULSEVIEW = 1 ]; then
    fetch pulseview $PULSEVIEW_REPO $PULSEVIEW_REV
    mkdir -p "$SRC/pulseview/build" && cd "$SRC/pulseview/build"
    cmake -DCMAKE_INSTALL_PREFIX="$PREFIX" -DCMAKE_BUILD_TYPE=Release .. >/dev/null
    make -s -j"$JOBS" && make -s install
fi

echo "Done: $PREFIX/bin/sigrok-cli -d litex-slogic[:conn=sim] ..."
