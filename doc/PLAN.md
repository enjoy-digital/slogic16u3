# SLogic16U3 Plan

Goal: an open alternative bitstream for the **Sipeed SLogic16U3** logic analyzer (Gowin GW5AT-15 with
the USB3 PHY in the FPGA transceivers), built with LiteX and [USB3-PIPE](https://github.com/enjoy-digital/usb3_pipe),
with low level C access and sigrok/PulseView and ngscopeclient on top.

## Process

Each step: **Implement -> Simulate (pytest/Migen, Verilator) -> Build (Gowin) -> Test on hardware
(scripted) -> Document -> Commit**. The Sipeed DFU loader (golden image at 0x800000) is never written:
the MODE button always gets back to it, the stock application can be restored at any time.

## Architecture

```
 CH0-CH15 --> Sampler (IDES8, 800MS/s base) --> Capture (test pattern, channel mode 4/8/16,
                                                  decimation /N, packer, sample limit)
                                                    |  sample clock domain (100MHz)
                                                 AsyncFIFO -> FIFO (BSRAM)
                                                    |  sys clock domain (62.5MHz, 64-bit)
 USB3 (GTR12 Q0, USB3-PIPE PHY + Device Core) <-- Bulk IN EP1: raw samples (sample-major, LSB=CH0)
                                              <-> EP0: CommUSB Wishbone bridge (CSRs: litex_server --usb)
 SPI Flash (MSPI) <-- Flash core (SPI engine, runtime pin mapping, page buffer): remote update
```

Host stack (layered, C first):

```
 libslogic (C: USB3 transport on libusb-1.0: EP0 CSR accesses, Bulk IN transfers ring; simulated device)
   -> slogic_util (info/check/bench/clock/regs/flash/dfu), slogic_record (raw/u8/VCD/CSV)
   -> libsigrok driver (litex-slogic): sigrok-cli, PulseView
   -> slogic_scopehal_bridge (TwinLan) + scopehal driver "slogic": ngscopeclient
 Python: litex_server --usb (CommUSB), test models.
```

## Phases

| #  | Phase                    | Content                                                                                               | Status            |
|----|--------------------------|-------------------------------------------------------------------------------------------------------|-------------------|
| 0  | Board support            | LiteX-Boards platform/target completed (inputs, LED, osc enable, multiboot to DFU), pinout findings.  | Done (build)      |
| 1  | GW5AT-15 USB3 SerDes     | LiteICLink GTR12_QUADB/UPARA, USB3-PIPE Q0/125MHz configuration (Gowin serdes_toml_to_csr).           | Done (build)      |
| 2  | Gateware cores           | Sampler (IDES8), Capture (pattern/decimation/modes/packer/limit/overflow), Analyzer, Flash; tests.    | Done (sim)        |
| 3  | SoC                      | CRG, USB3 (PHY + Core + EP0 Wishbone + Bulk IN), fixed CSR map, Gowin build (fit/timings).            | Fit, timings WIP  |
| 4  | System simulation        | USB3 host model <-> Device Core <-> SoC: CSR accesses and pattern captures over Bulk IN.              | Done (sim)        |
| 5  | C host stack             | libslogic (USB3/sim transports), slogic_util/slogic_record, DFU loader client, 175 C checks.          | Done (sim)        |
| 6  | sigrok / PulseView       | litex-slogic libsigrok driver, build script (libsigrok/libsigrokdecode/sigrok-cli/PulseView), tests.  | Done (sim)        |
| 7  | ngscopeclient            | slogic_scopehal_bridge (TwinLan) + scopehal driver "slogic" (syntax checked), bridge tests.           | Done (sim)        |
| 8  | Remote update            | Flash pads probe, update with verify (app region only), DFU loader client, recovery docs.             | Done (sim)        |
| 9  | Hardware bring-up        | USB3 link, pattern capture, inputs, sample clock accuracy, rates, throughput, update on hardware.     | Next              |
| 10 | Docs                     | README, GATEWARE/SOFTWARE/UPDATE/HARDWARE, promo video and stills.                                   | Done              |

## Timing Closure (GW5AT-15B, Gowin 1.9.12.04)

| Clock         | Target   | Full design (place_option 2) | place_option 1/3/4  | 1.9.10 (GW5AT-15A, place_option 2) |
|---------------|----------|------------------------------|---------------------|------------------------------------|
| sys           | 62.5MHz  | 71.9MHz                      | 67.1/93.9/69.4MHz   | 65.4MHz                            |
| tx (PIPE)     | 125MHz   | 139.3MHz                     | 129.1/133.0/137.0MHz| 133.6MHz                           |
| rx            | 125MHz   | 188.1MHz                     | 178.1/200.6/165.4MHz| 175.7MHz                           |
| sample        | 100MHz   | 106.3MHz                     | 101.6/105.0/100.2MHz| 100.8MHz                           |

Full design: 13.3K/15.1K logic (88%), 95% CLS, 26/35 BSRAM, 2/2 PLLs. Gowin 1.9.12: timings met with 4 of the 5
place algorithms (0: unrouted), place_option 2 with the best margins (default); 1.9.10 only met them with
place_option 2. Gowin PnR has no seed: `scripts/build_best.py` builds the place algorithms in parallel and keeps
the one with the best margins (as slogic16u3-trace). Next steps for robustness if needed: limit stage
pipelining (sample clock), MaxBurst 4.

## Hardware Bring-up Steps

1. `--no-analyzer --no-flash` bitstream over JTAG (`./slogic_16u3.py --build --load`): USB3 enumeration (`lsusb -t`:
   5000M, `SLogic16U3 LiteX`), `slogic_util info` (LTSSM U0), `litex_server --usb` CSR accesses.
   No link: retry with `--usb3-pll QPLL0` (the PLL of Gowin's USB 3.0 PHY IP on this board).
2. Full bitstream: `slogic_util check` (counter pattern, all modes/rates), `slogic_util bench` (throughput).
3. Inputs: IDES8 bit order (Q0 first, from the Gowin model), CH0-CH15 mapping, LVCMOS15UD33 threshold, 800MS/s
   on 4 channels.
4. `slogic_util clock`: sample clock accuracy (GTR12 TX clock source).
5. `slogic_util flash probe`: MSPI pads functions, then defaults in the Flash core/LiteX-Boards.
6. Updates: `slogic_util dfu update` (first install), `slogic_util flash update` (remote), MODE button recovery.
7. sigrok-cli/PulseView, ngscopeclient on the hardware.

## Flash Layout / Update

| Region            | Content                                                     | Written by                         |
|-------------------|-------------------------------------------------------------|------------------------------------|
| 0x000000-0x7fffff | Application bitstream (this project or the stock one).     | `slogic_util update`, DFU loader   |
| 0x800000-0xffffff | Sipeed DFU loader (golden image).                           | Never.                             |

- The bitstream is built with multiboot to 0x800000: the MODE button (RECONFIG_N) reloads the DFU loader.
- A corrupted application fails its check at power-up and the GW5AT falls back to the golden image (DFU).
- Update from our gateware: `slogic_util update app.bin` (erase/program/verify over EP0, replug to boot).
- Update from the DFU loader (stock or bricked unit): `slogic_util dfu write app.bin` (Sipeed bridge protocol).

## Open Questions (Hardware)

- SPI Flash MSPI pins functions (K13/K14/L14/L13/M14/M13): probed at runtime (`slogic_util flash probe`).
- Threshold voltage DAC (stock: 10-bit, `V/3.33/2*1024`, 0-6V): pin/interface to be identified.
- Fabric reconfiguration from the gateware (reboot after update without replug): SAMB + ? (to investigate).
- Sample clock source: GTR12 TX clock (125MHz oscillator, accurate) vs OSCA (internal, inaccurate).
- IDES8 at 800Mb/s on the LVCMOS15UD33 inputs (stock: 800MS/s on 4 channels).
- GTR12 lane PLL on the board: QPLL1 (default) or QPLL0 (`--usb3-pll QPLL0`, Gowin's USB 3.0 PHY IP choice).

## Ideas (beyond stock)

- Hardware triggers (edge/pattern/sequence) with pre-trigger in BSRAM, sample-exact trigger position.
- RLE compression (much longer captures on slow signals over the same USB bandwidth).
- External clock (state analysis) on CH12 (global clock input), clock qualifier.
- Pattern generator / output mode on spare channels (the inputs can be driven through the dividers).
- Frequency counter / timestamps per channel, glitch detection.
- Trace receiver mode (TPIU/PIB, as slogic16u3-trace), protocol offload (UART/SPI decoders in gateware).
- USB serial number from the GW5AT DNA (OTP), multiple units.
