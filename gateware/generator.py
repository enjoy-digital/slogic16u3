#
# This file is part of SLogic16U3.
#
# Copyright (c) 2026 Florent Kermarrec <florent@enjoy-digital.fr>
# SPDX-License-Identifier: BSD-2-Clause

"""Pattern Generator: protocol-like test signals at the base samplerate (Capture pattern 2), to verify
the full chain (decimation, channel modes, packing, USB3, host unpacking, sigrok decoders) without
probes.

The generator restarts with each capture (held at its origin while `enable` is low): sample t of the
capture (t: base sample index, 800MS/s with ratio=8 at 100MHz, n = t//8: cycle, j = t%8: sample in the
cycle) is a pure function of t, so the host checks every captured sample (libslogic
slogic_generator_sample(), the same model):

- CH0     : UART TX 8N1 at 1Mbaud (100 cycles per bit), GENERATOR_TEXT repeated, 12 bits frames: 2 idle
            bits (the capture starts idle: decoders synchronize on the first character), start, 8 data
            bits (LSB first), stop: frame = n//1200, character = frame % len, bit = (n//100) % 12.
- CH1-CH3 : SPI mode 0 (CS#, SCK, MOSI, MSB first) at 12.5MHz: n % 512 < 256: CS# low, SCK = n[2],
            MOSI = GENERATOR_SPI[n[6:8]] bit 7 - n[3:6]; else CS# high, SCK/MOSI low.
- CH4     : PWM, 1024 cycles period (~97.7kHz): (n % 1024) < 4*((n >> 10) % 256) (duty sweep).
- CH5     : 100MHz clock: j >= 4.
- CH6-CH15: 10-bit Gray counter (n % 1024 in Gray code).
n is 18-bit (wraps every 2^18 cycles).
"""

from migen import *

from litex.gen import *

# Constants ----------------------------------------------------------------------------------------

GENERATOR_TEXT      = "SLogic16U3 LiteX pattern generator\r\n"
GENERATOR_SPI       = [0x53, 0x4c, 0x4f, 0x47] # "SLOG".
GENERATOR_UART_BAUD = 100 # Cycles per UART bit (1Mbaud at 100MHz).
GENERATOR_UART_BITS = 12  # 2 idle bits, start, 8 data bits, stop.
GENERATOR_CYC_BITS  = 18

# Pattern Generator --------------------------------------------------------------------------------

class PatternGenerator(LiteXModule):
    def __init__(self, nchannels=16, ratio=8):
        assert nchannels == 16
        assert ratio == 8
        self.enable = Signal()                 # i: Running (low: held at the origin, sample 0).
        self.word   = Signal(ratio*nchannels)  # o: Samples of the cycle (registered), first on the LSBs.

        # # #

        # State of the output word (cycle n) and next state (n + 1, or the origin when disabled): the
        # output word is registered from the next state, so it holds sample 0 on the first enabled cycle.
        L      = len(GENERATOR_TEXT)
        n      = Signal(GENERATOR_CYC_BITS)
        baud   = Signal(max=GENERATOR_UART_BAUD)
        bit    = Signal(max=GENERATOR_UART_BITS)
        char   = Signal(max=L)
        n_n    = Signal(GENERATOR_CYC_BITS)
        baud_n = Signal(max=GENERATOR_UART_BAUD)
        bit_n  = Signal(max=GENERATOR_UART_BITS)
        char_n = Signal(max=L)
        self.comb += [
            If(~self.enable,
                n_n.eq(0),
                baud_n.eq(0),
                bit_n.eq(0),
                char_n.eq(0),
            ).Else(
                n_n.eq(n + 1),
                If(baud == GENERATOR_UART_BAUD - 1,
                    baud_n.eq(0),
                    If(bit == GENERATOR_UART_BITS - 1,
                        bit_n.eq(0),
                        char_n.eq(Mux(char == L - 1, 0, char + 1)),
                    ).Else(
                        bit_n.eq(bit + 1),
                        char_n.eq(char),
                    )
                ).Else(
                    baud_n.eq(baud + 1),
                    bit_n.eq(bit),
                    char_n.eq(char),
                )
            )
        ]
        self.sync += [
            n.eq(n_n),
            baud.eq(baud_n),
            bit.eq(bit_n),
            char.eq(char_n),
        ]

        # Channels (from the next state, registered with it).
        text = Array(Constant(ord(c), 8) for c in GENERATOR_TEXT)
        # SPI frame bits (MSB first).
        spi  = Array(Constant((GENERATOR_SPI[i//8] >> (7 - i%8)) & 1, 1) for i in range(32))
        byte = Signal(8)
        uart = Signal()
        cs_n = Signal()
        sck  = Signal()
        mosi = Signal()
        pwm  = Signal()
        gray = Signal(10)
        self.comb += [
            byte.eq(text[char_n]),
            Case(bit_n, {
                2         : uart.eq(0),                                      # Start.
                **{b + 3  : uart.eq(byte[b]) for b in range(8)},             # Data, LSB first.
                "default" : uart.eq(1),                                      # Idle/stop.
            }),
            cs_n.eq(n_n[8]),
            sck.eq(~n_n[8] & n_n[2]),
            mosi.eq(~n_n[8] & spi[n_n[3:8]]),
            pwm.eq(n_n[0:10] < Cat(C(0, 2), n_n[10:18])),
            gray.eq(n_n[0:10] ^ n_n[1:10]),
        ]
        channels = Cat(uart, cs_n, sck, mosi, pwm, C(0, 1), gray) # CH5 (clock) set per sample below.
        self.sync += self.word.eq(Cat(*[channels | (int(j >= 4) << 5) for j in range(ratio)]))

# Reference Model ----------------------------------------------------------------------------------

def generator_sample(t):
    """16-bit sample t (base sample index from the capture start) of the pattern generator."""
    n, j = (t // 8) % 2**GENERATOR_CYC_BITS, t % 8
    char = GENERATOR_TEXT[(t // 8 // (GENERATOR_UART_BAUD*GENERATOR_UART_BITS)) % len(GENERATOR_TEXT)]
    bit  = (t // 8 // GENERATOR_UART_BAUD) % GENERATOR_UART_BITS
    uart = 0 if bit == 2 else (ord(char) >> (bit - 3)) & 1 if 3 <= bit <= 10 else 1
    m    = n % 512
    cs_n = int(m >= 256)
    sck  = 0 if cs_n else (n >> 2) & 1
    mosi = 0 if cs_n else (GENERATOR_SPI[(n >> 6) & 3] >> (7 - ((n >> 3) & 7))) & 1
    pwm  = int((n % 1024) < 4*((n >> 10) % 256))
    clk  = int(j >= 4)
    gray = (n % 1024) ^ ((n % 1024) >> 1)
    return uart | cs_n << 1 | sck << 2 | mosi << 3 | pwm << 4 | clk << 5 | gray << 6
