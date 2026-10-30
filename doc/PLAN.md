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
| 5  | C host stack             | libslogic (USB3/sim transports), slogic_util/slogic_record, DFU loader client, C tests.               | Done (sim)        |
| 6  | sigrok / PulseView       | litex-slogic libsigrok driver, build script (libsigrok/libsigrokdecode/sigrok-cli/PulseView), tests.  | Done (sim)        |
| 7  | ngscopeclient            | slogic_scopehal_bridge (TwinLan) + scopehal driver "slogic" (syntax checked), bridge tests.           | Done (sim)        |
| 8  | Remote update            | Flash pads probe, update with verify (app region only), DFU loader client, recovery docs.             | Done (sim)        |
| 9  | Hardware bring-up        | USB3 link, pattern capture, inputs, sample clock accuracy, rates, throughput, update on hardware.     | In progress       |
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

1. **Done**: USB3 enumeration (5000M, `SLogic16U3 LiteX`, descriptors/strings), EP0 CSR accesses (1000/1000
   write/read), `slogic_util info` (U0). Findings:
   - QPLL1: no link (host sees no device); QPLL0 (Gowin's USB 3.0 PHY IP choice): default.
   - CDR/QPLL0 settings from Gowin's USB 3.0 PHY IP configuration (`GW5AT15_USB3_CSR_OVERRIDES`): with the
     serdes_toml_to_csr CDR value, RX 8b/10b errors prevent the training (Polling.Idle timeouts).
   - USB3-PIPE DPP RX adapter FIFO (asynchronous RAM read) mis-implemented by Gowin in block RAMs (EP0 Setups
     read back as zeros): FIFO buffered in USB3-PIPE, build check in `gowin_distributed_memories`.
   - Bring-up debug: `--with-jtagbone --with-usb3-debug` (LTSSM history, GTR12 status/UPAR, link statistics,
     Data Packets rings) over the JTAG port, `software/debug/usb3_debug.py`.
2. **Done** (full bitstream over JTAG): `slogic_util check` 99/99 (counter pattern, all modes, 1kHz to 400MB/s,
   no errors), `slogic_util bench` 400MB/s sustained (30s) at 4ch@800MHz, 8ch@400MHz and 16ch@200MHz. Findings:
   - 32-bit capture CDC to sys (250MB/s at 62.5MHz): 64-bit words converted in the sample clock domain.
   - Bulk IN at ~370MB/s with a Max Burst of 8 (`--with-jtagbone --with-usb3-counters`: device waiting for a host
     request ~50% of the time, ACK TPs NumP counted down from Max Burst by the AMD xHCI): Max Burst 16, capture
     FIFO 2048x64 (16KB) against the host gaps.
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
- 320/400MB/s captures: capture FIFO depth (BSRAMs left: ~9 x 18Kb), Bulk IN/Link Layer efficiency on the
  GW5AT-15 (bench: overflow at the start).
- Generate the CDR/QPLL0 settings from the TOML (serdes_toml_to_csr keys) instead of overrides.

## Ideas (beyond stock)

- Hardware triggers (edge/pattern/sequence) with pre-trigger in BSRAM, sample-exact trigger position.
- RLE compression (much longer captures on slow signals over the same USB bandwidth).
- External clock (state analysis) on CH12 (global clock input), clock qualifier.
- Output mode on spare channels (the inputs can be driven through the dividers; the internal pattern
  generator is a capture source only).
- Frequency counter / timestamps per channel, glitch detection.
- Trace receiver mode (TPIU/PIB, as slogic16u3-trace), protocol offload (UART/SPI decoders in gateware).
- USB serial number from the GW5AT DNA (OTP), multiple units.
