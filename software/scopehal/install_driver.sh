#!/bin/sh
#
# This file is part of SLogic16U3.
#
# Copyright (c) 2026 Enjoy-Digital <enjoy-digital.fr>
# SPDX-License-Identifier: BSD-2-Clause
#
# Add the SLogicOscilloscope driver to a libscopehal source tree (ngscopeclient checkout: lib/scopehal):
#   ./install_driver.sh ~/ngscopeclient/lib && (rebuild ngscopeclient)

set -e
HERE=$(cd "$(dirname "$0")" && pwd)
LIB=${1:?usage: install_driver.sh <scopehal library dir (contains scopehal/scopehal.cpp)>}
S=$LIB/scopehal
[ -f "$S/scopehal.cpp" ] || { echo "$S/scopehal.cpp not found"; exit 1; }
cp "$HERE"/scopehal/SLogicOscilloscope.h "$HERE"/scopehal/SLogicOscilloscope.cpp "$S/"
grep -q "SLogicOscilloscope.cpp" "$S/CMakeLists.txt" || \
    sed -i 's/^\(\s*\)DSLabsOscilloscope.cpp/&\n\1SLogicOscilloscope.cpp/' "$S/CMakeLists.txt"
grep -q '"SLogicOscilloscope.h"' "$S/scopehal.cpp" || \
    sed -i 's/^#include "DSLabsOscilloscope.h"/&\n#include "SLogicOscilloscope.h"/' "$S/scopehal.cpp"
grep -q "AddDriverClass(SLogicOscilloscope)" "$S/scopehal.cpp" || \
    sed -i 's/^\(\s*\)AddDriverClass(DSLabsOscilloscope);/&\n\1AddDriverClass(SLogicOscilloscope);/' "$S/scopehal.cpp"
echo "SLogicOscilloscope added to $S (driver \"slogic\", transport \"twinlan\")."
