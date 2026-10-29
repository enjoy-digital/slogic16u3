# Gateware

One LiteX SoC ([`slogic_16u3.py`](../slogic_16u3.py)), no CPU: the host drives everything over USB3 (EP0 CSR
accesses, Bulk IN data).

```
                 sample_fast (400MHz, DDR)      sample (100MHz)                       sys (62.5MHz)
 CH0-CH15 ──► IDES8 x16 ──► Capture ──────────────────────► CDC ──► FIFO (512x64) ──► USB3 Bulk IN (EP1)
              (Sampler)     source/decimation/width/packer                              │
                                                                                         ▼
 SPI Flash ◄── Flash core ◄── CSRs/buffers ◄── Wishbone ◄── EP0 CommUSB bridge ◄── USB3 Device Core
  (MSPI)      (runtime pins)                                                             │  Link/Protocol layers
                                                                                         ▼
                                                    USB3-PIPE PHY: LTSSM (sys) + PIPE (tx: 125MHz) ◄─► GTR12 Q0 ◄─► USB-C
```

## Clocks

| Domain        | Frequency | Source                                                                 |
|---------------|-----------|------------------------------------------------------------------------|
| `por`         | ~51.8MHz  | OSCA (internal oscillator), power-on reset.                            |
| `sys`         | 62.5MHz   | PLL from OSCA: USB3 LTSSM/Link/Protocol layers, SoC, Flash.            |
| `tx`/`rx`     | 125MHz    | GTR12 PCS fabric clocks (125MHz oscillator on Q0 REFCLK0): PIPE.       |
| `sample_fast` | 400MHz    | PLL from the GTR12 TX clock (accurate samplerate): IDES8 FCLK (DDR).   |
| `sample`      | 100MHz    | CLKDIV /4 of `sample_fast`: IDES8 PCLK, Capture.                       |

`--sample-clk-source osc` feeds the sample PLL from OSCA instead (no SerDes dependency, internal oscillator
accuracy). `slogic_util clock` measures the sample clock against the host clock.

## Cores

| Core                         | File                                       | Notes                                                                 |
|------------------------------|--------------------------------------------|-----------------------------------------------------------------------|
| CRG                          | [crg.py](../gateware/crg.py)               | OSCA, POR, sys PLL, sample PLL + CLKDIV.                              |
| Sampler                      | [sampler.py](../gateware/sampler.py)       | IDES8 per input: 8 samples per sample clock cycle (Q0 first).         |
| Capture                      | [capture.py](../gateware/capture.py)       | Test pattern, decimation /N (any N), 4/8/16 channels, 32-bit packer, limit, overflow. |
| Analyzer                     | [analyzer.py](../gateware/analyzer.py)     | Capture + CDC + FIFO, info/control/status CSRs, sample clock counter. |
| USB3 Device                  | [usb3.py](../gateware/usb3.py)             | USB3-PIPE Device Core: EP0 Wishbone bridge, Bulk IN (64-bit).         |
| Flash                        | [flash.py](../gateware/flash.py)           | SPI master, runtime pads mapping, 512-byte TX/RX buffers.             |

## Capture Datapath

- Base rate 800MS/s (8 samples per 100MHz cycle), samplerate = 800MHz/divider (stock rates: 5-800MHz).
- Decimation: one sample every `divider`, the first sample of the capture kept, several samples per cycle
  compacted below divider 8; single cycle recurrence (table for dividers < 8, one adder above).
- Up to 32 bits per cycle (400MB/s): 4ch@800MS/s, 8ch@400MS/s, 16ch@200MS/s (more: `error`, capture stopped).
- Wire format: sample-major, first sample on the LSBs (4ch: 2 samples per byte), 64-bit words on the
  Bulk IN endpoint, the last word flushed (padded) with a short packet.
- Limit (48-bit) or continuous until stopped, overflow (FIFO full: data lost) stops the capture.
- Counter test pattern: sample i = i x divider (16-bit, masked): end-to-end checks without probes
  (`slogic_util check`).

## Registers

Fixed CSR map ([slogic_regs.h](../software/user/libslogic/slogic_regs.h), checked against the generated
`csr.csv` by `test/test_regs.py`):

| Bank           | Base   | Registers                                                                       |
|----------------|--------|---------------------------------------------------------------------------------|
| ctrl           | 0x0000 | reset, scratch, bus_errors.                                                     |
| analyzer       | 0x0800 | magic ("SLOG"), version, config, base_rate, control, divider, limit (64), status, count (64), clk_count. |
| flash          | 0x1000 | control (enable/pads/high), divider, xfer (length/hold), ctrl (start/release), status. |
| usb3_phy       | 0x1800 | status (LTSSM state, U0, ready).                                                |
| identifier_mem | 0x2000 | SoC identifier (one character per word).                                        |
| flash_buffer   | 0x20000000 | 512-byte TX (writes)/RX (reads) buffers.                                    |

## Resources/Timings (Gowin 1.9.10, GW5AT-LV15MG132C1)

The design fills most of the GW5AT-15: the USB3 PHY + Device Core alone use ~70% of the logic (64-bit Link
Layer at 62.5MHz). BSRAMs are kept for the deep buffers (USB3 packets slots, capture FIFO), shallow and
initialized memories go to distributed RAM (`gowin_distributed_memories`: Gowin 1.9.10 also rejects initialized
BSRAMs on the GW5AT-15A).

| Build                          | Logic       | BSRAM | sys (62.5) | tx (125)  | sample (100) |
|--------------------------------|-------------|-------|------------|-----------|--------------|
| USB3 only (`--no-analyzer --no-flash`) | 10368 (69%) | 86%¹ | 83.4MHz | 126.9MHz | -          |
| Full (place_option 2)          | 13443 (89%) | 80%   | 65.4MHz    | 133.6MHz  | 100.8MHz     |

¹ USB3 packets slots optimized out without the Analyzer.

Gowin PnR has no seed and the design is ~90% full: `scripts/build_best.py` builds several place algorithms in
parallel and keeps one meeting the timings.

## Simulation

- `test/test_capture.py`, `test/test_flash.py`: cores against reference models (Migen).
- `test/test_system.py`: USB3 host model (USB3-PIPE) ↔ Device Core (PHY stand-in, 64-bit Link Layer) ↔ the
  same SoC as the hardware target: CSR accesses over EP0, counter pattern captures over Bulk IN.
