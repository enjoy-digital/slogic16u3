#
# This file is part of SLogic16U3.
#
# Copyright (c) 2026 Florent Kermarrec <florent@enjoy-digital.fr>
# SPDX-License-Identifier: BSD-2-Clause

"""Test helpers: capture reference model (decimation, channel mode, packing)."""

MODE_BITS = {0: 4, 1: 8, 2: 16}

def capture_reference(samples, divider, mode, limit=None):
    """Captured samples (decimated, masked to the channel mode) from a flat list of 16-bit samples."""
    bits = MODE_BITS[mode]
    kept = [s & (2**bits - 1) for s in samples[::divider]]
    return kept if limit is None else kept[:limit]

def pack(samples, mode, data_width=64):
    """Packed data words (sample-major, first sample on the LSBs), last word padded with zeros."""
    bits  = MODE_BITS[mode]
    value = 0
    for i, s in enumerate(samples):
        value |= s << (i*bits)
    nbits = len(samples)*bits
    n     = (nbits + data_width - 1)//data_width
    return [(value >> (data_width*i)) & (2**data_width - 1) for i in range(n)]

def unpack(words, mode, count, data_width=64):
    """Samples from packed data words."""
    bits  = MODE_BITS[mode]
    value = 0
    for i, w in enumerate(words):
        value |= w << (data_width*i)
    return [(value >> (i*bits)) & (2**bits - 1) for i in range(count)]
