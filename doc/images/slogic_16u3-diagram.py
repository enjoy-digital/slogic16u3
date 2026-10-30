#!/usr/bin/env python3

#
# This file is part of SLogic16U3.
#
# Copyright (c) 2026 Enjoy-Digital <enjoy-digital.fr>
# SPDX-License-Identifier: BSD-2-Clause

"""slogic_16u3-diagram.svg: the SLogic16U3 gateware in the classic LiteX diagram style (white canvas,
chamfered boxes, grey arrows, the EnjoyDigital and "Built with LiteX" logos embedded from this
directory).

    python3 doc/images/slogic_16u3-diagram.py [--boxes boxes.json]   # -> slogic_16u3-diagram.svg
    inkscape slogic_16u3-diagram.svg --export-type=png --export-filename=slogic_16u3-diagram.png --export-width=2800

The SVG stays editable by hand (Inkscape or XML): the generator only keeps the geometry consistent.
--boxes also writes the boxes geometry (title, x1, y1, x2, y2) as JSON: the video highlights them.
Layout rules: arrows leave and enter boxes at their centre lines, boxes of one row/column share
their edges, every label has room (Carlito/Calibri metrics) between the things it sits between.
"""

import os
import json
import base64
import argparse

FONT = "Calibri, Carlito, 'Liberation Sans', Arial, Helvetica, sans-serif"
STROKE, TEXT, ARROW = "#7f7f7f", "#1a1a1a", "#595959"
MEM, HARD, FPGA, BOARD = "#c9c9c9", "#dcdcdc", "#e4e4e4", "#f8f8f8"
KEY = "#cdcdcd" # The USB3 path in the FPGA transceivers (no USB chip).
out   = []
boxes = []      # Every box drawn: (title, x1, y1, x2, y2).

# Helpers ------------------------------------------------------------------------------------------

def esc(s):
    return s.replace("&", "&amp;").replace("<", "&lt;").replace(">", "&gt;")

def box(x1, y1, x2, y2, fill="#ffffff", c=14, lines=(), shadow=True, dash=False):
    pts   = f"{x1},{y1} {x2-c},{y1} {x2},{y1+c} {x2},{y2} {x1+c},{y2} {x1},{y2-c}"
    extra = ' stroke-dasharray="7,4"' if dash else ""
    filt  = ' filter="url(#shadow)"' if shadow else ""
    out.append(f'  <polygon points="{pts}" fill="{fill}" stroke="{STROKE}" stroke-width="1.5"{extra}{filt}/>')
    title = (lines[0][0] if isinstance(lines[0], tuple) else lines[0]) if lines else ""
    boxes.append((title, x1, y1, x2, y2))
    if lines:
        text_lines((x1 + x2)/2, (y1 + y2)/2, lines)

def text_lines(cx, cy, lines, anchor="middle"):
    # lines: (text, size, weight, style), weight/style optional: a vertically centred block.
    norm = []
    for l in lines:
        l = (l + (None,)*4)[:4] if isinstance(l, tuple) else (l, 13, None, None)
        norm.append((l[0], l[1] or 13, l[2], l[3]))
    heights = [s*1.3 for _, s, _, _ in norm]
    y = cy - sum(heights)/2
    for (t, s, w, st), h in zip(norm, heights):
        text(cx, y + h*0.5 + s*0.35, t, s, anchor=anchor, weight=w, style=st)
        y += h

def text(x, y, t, size=13, anchor="middle", weight=None, style=None, fill=TEXT, rotate=None):
    attrs = f'x="{x}" y="{y}" font-size="{size}" text-anchor="{anchor}" fill="{fill}"'
    if weight: attrs += f' font-weight="{weight}"'
    if style:  attrs += f' font-style="{style}"'
    if rotate: attrs += f' transform="rotate({rotate} {x} {y})"'
    out.append(f'  <text {attrs}>{esc(t)}</text>')

def head(x, y, dx, dy, col, size=9):
    # A filled triangle with its tip at (x, y), pointing along (dx, dy).
    px, py = -dy, dx
    bx, by = x - dx*size, y - dy*size
    hw = size*0.42
    out.append(f'  <polygon points="{x:.1f},{y:.1f} {bx+px*hw:.1f},{by+py*hw:.1f} {bx-px*hw:.1f},{by-py*hw:.1f}" fill="{col}"/>')

def arrow(pts, double=False, heads=True):
    size = 9
    p = [list(q) for q in pts]
    def unit(a, b):
        dx, dy = b[0] - a[0], b[1] - a[1]
        n = (dx*dx + dy*dy) ** 0.5
        return dx/n, dy/n
    hs = []
    if heads:
        dx, dy = unit(p[-2], p[-1]); hs.append((p[-1][0], p[-1][1], dx, dy))
        p[-1] = [p[-1][0] - dx*(size - 2), p[-1][1] - dy*(size - 2)]
        if double:
            dx, dy = unit(p[1], p[0]); hs.append((p[0][0], p[0][1], dx, dy))
            p[0] = [p[0][0] - dx*(size - 2), p[0][1] - dy*(size - 2)]
    d = "M " + " L ".join(f"{x:.1f},{y:.1f}" for x, y in p)
    out.append(f'  <path d="{d}" fill="none" stroke="{ARROW}" stroke-width="1.6" stroke-linejoin="round"/>')
    for x, y, dx, dy in hs:
        head(x, y, dx, dy, ARROW, size)

def dot(x, y):
    out.append(f'  <circle cx="{x}" cy="{y}" r="3.5" fill="{ARROW}"/>')

def dashed(x1, x2, y, col=STROKE):
    out.append(f'  <line x1="{x1}" y1="{y}" x2="{x2}" y2="{y}" stroke="{col}" stroke-width="1.2" stroke-dasharray="5,4"/>')

def image(path, x, y, w, h):
    data = base64.b64encode(open(path, "rb").read()).decode()
    out.append(f'  <image x="{x}" y="{y}" width="{w}" height="{h}" xlink:href="data:image/png;base64,{data}"/>')

def begin(W, H, name, title, desc):
    out.clear()
    boxes.clear()
    out.append(f'<svg xmlns="http://www.w3.org/2000/svg" xmlns:xlink="http://www.w3.org/1999/xlink" width="{W}" height="{H}" viewBox="0 0 {W} {H}" role="img" aria-labelledby="{name}-title {name}-description">')
    out.append(f'  <title id="{name}-title">{esc(title)}</title>')
    out.append(f'  <desc id="{name}-description">{esc(desc)}</desc>')
    out.append('  <!-- Editable SVG, classic LiteX diagram style: chamfered boxes, grey arrows, light fills. Generated by doc/images/slogic_16u3-diagram.py; edit either. -->')
    out.append('  <defs>')
    out.append('    <filter id="shadow" x="-10%" y="-10%" width="125%" height="130%"><feGaussianBlur in="SourceAlpha" stdDeviation="2.4" result="b"/><feOffset in="b" dx="3" dy="3" result="o"/><feComponentTransfer in="o" result="s"><feFuncA type="linear" slope="0.32"/></feComponentTransfer><feMerge><feMergeNode in="s"/><feMergeNode in="SourceGraphic"/></feMerge></filter>')
    out.append('  </defs>')
    out.append(f'  <g id="{name}" font-family="{FONT}">')
    out.append(f'  <rect width="{W}" height="{H}" fill="#ffffff"/>')

def finish(path):
    out.append('  </g>')
    out.append('</svg>')
    open(path, "w", encoding="utf-8").write("\n".join(out) + "\n")
    print(path)

# Diagram ------------------------------------------------------------------------------------------
#
# Columns: probes 10..150 | board left 185..315 | FPGA 330..1275, data row boxes 130 wide every 152
# from 355 (Sampler, Capture, CDC + FIFO, USB3 Core, USB3 PHY, GTR12) | board right 1295..1425 |
# host 1460..1590. Rows: data path y 112..212 (centre 162) | control 300..380 | CSR bus 420 | CRG,
# FPGA name 465..580 | board parts below the FPGA 635..705.

TITLE = "SLogic16U3: open LiteX gateware for the Sipeed USB3 logic analyzer"
DESC  = ("16 inputs sampled by IDES8 deserializers at 800 MS/s, captured (test pattern, decimation, 4/8/16 "
         "channels, packing to 64-bit words), crossed to the 62.5 MHz system clock and buffered, then sent "
         "over USB3 Bulk IN by the USB3-PIPE Device Core and PHY through the GW5AT-15 GTR12 transceiver at "
         "5 Gb/s, with no USB chip and no CPU. The host drives everything over EP0 (CSR accesses through a "
         "Wishbone bridge): the analyzer configuration and the Flash core (remote update).")

X  = [355 + i*152 for i in range(6)]  # Data row boxes (130 wide).
CX = [x + 65 for x in X]              # Their centres.
YR = 162                              # Data row centre.

def draw(logos):
    begin(1600, 885, "slogic_16u3-diagram", TITLE, DESC)

    # Board and FPGA boundaries.
    box(170, 25, 1445, 755, fill=BOARD, c=34)
    box(330, 55, 1275, 600, fill=FPGA, c=30)
    text(1430, 743, "Sipeed SLogic16U3 · USB3 logic analyzer", 17, anchor="end", style="italic")
    text(355, 548, "Gowin GW5AT-15", 24, anchor="start", weight="bold", style="italic")
    text(355, 574, "LiteX SoC · no CPU · USB3 in the FPGA transceivers", 15, anchor="start", style="italic")

    # Outside the board: the probes and the host.
    box(10, 118, 150, 206, lines=[("Probes", 18, "bold"), ("CH0-CH15", 14), ("signals under test", 13)])
    arrow([(150, YR), (185, YR)])
    box(1460, 90, 1590, 234, lines=[("Host PC", 18, "bold"), ("libslogic", 13), ("sigrok · PulseView", 13),
                                    ("ngscopeclient", 13)])

    # On the board.
    box(185, 112, 315, 212, lines=[("2x10 header", 16, "bold"), ("CH0-CH15 · GND", 13), ("0.47 dividers", 13)])
    arrow([(315, YR), (X[0], YR)])
    box(185, 300, 315, 380, fill=MEM, lines=[("SPI flash", 16, "bold"), ("16 MB", 13), ("gateware · DFU", 13)])
    box(1295, 122, 1425, 202, lines=[("VL162", 16, "bold"), ("Type-C mux", 13), ("USB-C", 13)])
    arrow([(X[5] + 130, YR), (1295, YR)], double=True)
    arrow([(1425, YR), (1460, YR)], double=True)
    text(1442, 150, "USB3", 12, style="italic")
    box(1115, 635, 1245, 705, fill=MEM, lines=[("125 MHz osc", 16, "bold"), ("GTR12 REFCLK0", 13)])

    # Clock domains (above the data row).
    for x1, x2, label in [(X[0], X[1] + 130, "sample · 100 MHz (400 MHz DDR)"), (X[2], X[3] + 130, "sys · 62.5 MHz · 64-bit"),
                          (X[4], X[5] + 130, "tx · 125 MHz")]:
        dashed(x1, x2, 92)
        text((x1 + x2)/2, 84, label, 13, style="italic")

    # Data path.
    box(X[0], 112, X[0] + 130, 212, lines=[("Sampler", 16, "bold"), ("IDES8 × 16", 13), ("800 MS/s", 13)])
    box(X[1], 112, X[1] + 130, 212, lines=[("Capture", 16, "bold"), ("pattern · /N", 13), ("4/8/16 ch · pack", 13),
                                           ("64-bit words", 13)])
    box(X[2], 112, X[2] + 130, 212, lines=[("CDC + FIFO", 16, "bold"), ("sample → sys", 13), ("2048 × 64 · 16 KB", 13)])
    box(X[3], 112, X[3] + 130, 212, fill=KEY, lines=[("USB3 Core", 16, "bold"), ("Link · Protocol", 13),
                                                     ("Bulk IN · burst 16", 13)])
    box(X[4], 112, X[4] + 130, 212, fill=KEY, lines=[("USB3 PHY", 16, "bold"), ("LTSSM · PIPE", 13), ("USB3-PIPE", 13)])
    box(X[5], 112, X[5] + 130, 212, fill=HARD, lines=[("GTR12", 16, "bold"), ("SerDes Q0", 13), ("5 Gb/s · QPLL0", 13)])
    for i in range(3):
        arrow([(X[i] + 130, YR), (X[i + 1], YR)])
    for i in (3, 4):
        arrow([(X[i] + 130, YR), (X[i + 1], YR)], double=True)

    # Control: EP0 bridge and JTAGBone (masters), analyzer CSRs and Flash core on the CSR bus.
    box(X[0], 300, X[0] + 130, 380, lines=[("Flash core", 16, "bold"), ("MSPI · pads probe", 13), ("remote update", 13)])
    box(X[1], 300, X[1] + 130, 380, lines=[("Analyzer CSRs", 16, "bold"), ("mode · rate · limit", 13), ("status · counters", 13)])
    box(X[3], 300, X[3] + 130, 380, fill=KEY, lines=[("EP0 bridge", 16, "bold"), ("CommUSB", 13), ("Wishbone master", 13)])
    box(X[4], 300, X[4] + 130, 380, dash=True, lines=[("JTAGBone", 16, "bold"), ("optional", 13, None, "italic"),
                                                      ("debug · counters", 13)])
    arrow([(CX[1], 212), (CX[1], 300)], double=True)
    text(CX[1] - 8, 262, "config · status", 13, anchor="end", style="italic")
    arrow([(CX[3], 212), (CX[3], 300)], double=True)
    text(CX[3] - 8, 262, "EP0 · control", 13, anchor="end", style="italic")
    arrow([(X[0], 340), (315, 340)], double=True)
    # CSR bus.
    arrow([(CX[0], 420), (CX[4], 420)], heads=False)
    for x in (CX[0], CX[1], CX[3], CX[4]):
        arrow([(x, 420), (x, 380)], double=True)
        dot(x, 420)
    text((CX[1] + CX[3])/2, 442, "Wishbone · CSR bus", 13, style="italic")

    # Clocking.
    box(X[4], 465, X[4] + 145, 545, lines=[("CRG", 16, "bold"), ("sys PLL from OSCA", 13), ("sample PLL from TX", 13)])
    arrow([(CX[5] + 20, 635), (CX[5] + 20, 212)])
    text(CX[5] + 28, 580, "REFCLK", 12, anchor="start", style="italic")

    # Caption and logos.
    cx = 800
    text(cx, 803, TITLE, 26, weight="bold", style="italic")
    text(cx, 832, "16 channels · 4ch@800 / 8ch@400 / 16ch@200 MS/s · 400 MB/s sustained over USB3 5 Gb/s", 15, style="italic")
    text(cx, 855, "USB3 in the FPGA transceivers (USB3-PIPE): no USB chip, no CPU · remote update over USB3", 15, style="italic")
    image(os.path.join(logos, "enjoy-digital-logo.png"), 18, 765, 110, 110)
    text(1432, 837, "Built with", 19, anchor="end")
    image(os.path.join(logos, "litex-logo.png"), 1440, 787, 148, 85)

def main():
    here = os.path.dirname(os.path.abspath(__file__))
    parser = argparse.ArgumentParser(description="SLogic16U3 gateware diagram.")
    parser.add_argument("--logos", default=here, help="Directory with enjoy-digital-logo.png and litex-logo.png.")
    parser.add_argument("--out",   default=here, help="Output directory.")
    parser.add_argument("--boxes", default=None, help="Also write the boxes geometry as JSON (video highlights).")
    args = parser.parse_args()
    draw(args.logos)
    finish(os.path.join(args.out, "slogic_16u3-diagram.svg"))
    if args.boxes:
        json.dump([dict(title=t, x1=x1, y1=y1, x2=x2, y2=y2) for t, x1, y1, x2, y2 in boxes if t],
            open(args.boxes, "w"), indent=1)

if __name__ == "__main__":
    main()
