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
 libliteusb (libusb-1.0: EP0 CSR accesses, Bulk IN stream ring)
   -> libslogic (C API: open/info/configure/capture/stream, flash update, DFU loader client)
      -> slogic_util (info/regs/flash/update/dfu), slogic_record (capture to file), slogic_check (self-test)
      -> libsigrok driver (litex-slogic): sigrok-cli, PulseView
      -> slogic_scopehal_bridge (TwinLan) + scopehal driver: ngscopeclient
 Python: software/slogic.py (bring-up, litex_server/RemoteClient), test models.
```

## Phases

| #  | Phase                    | Content                                                                                               | Status       |
|----|--------------------------|-------------------------------------------------------------------------------------------------------|--------------|
| 0  | Board support            | LiteX-Boards platform/target completed (inputs, LED, osc enable, multiboot to DFU), pinout findings.  | Done (build) |
| 1  | GW5AT-15 USB3 SerDes     | LiteICLink GTR12_QUADB/UPARA, USB3-PIPE Q0/125MHz configuration (Gowin serdes_toml_to_csr).           | Done (build) |
| 2  | Gateware cores           | Sampler, Capture (pattern/mode/decimation/packer), Streamer, Flash, Info; unit tests.                 |              |
| 3  | SoC                      | CRG, USB3 (PHY + Core + EP0 Wishbone + Bulk IN), fixed CSR map, Gowin build (fit/timings).            |              |
| 4  | System simulation        | USB3 host model <-> Device Core <-> SoC: CSR accesses and pattern captures over Bulk IN.              |              |
| 5  | C host stack             | libliteusb, libslogic, tools, unit tests (mock transport, sample unpacking, DFU/flash protocols).     |              |
| 6  | sigrok / PulseView       | libsigrok driver, build script (libsigrok/sigrok-cli/PulseView), tests on a simulated device.         |              |
| 7  | ngscopeclient            | TwinLan bridge + scopehal driver.                                                                     |              |
| 8  | Remote update            | Flash pin mapping probe, update (erase/program/verify app region), DFU loader client, recovery.       |              |
| 9  | Hardware bring-up        | USB3 link, pattern capture, inputs, sample clock accuracy, rates, throughput, update on hardware.     |              |
| 10 | Docs                     | README, diagrams, promo video, getting started, recovery.                                             |              |

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
- Gowin >= 1.9.12 (GW5AT-15 version B, initialized BSRAMs): 1.9.10 rejects initialized BSRAMs.

## Ideas (beyond stock)

- Hardware triggers (edge/pattern/sequence) with pre-trigger in BSRAM, sample-exact trigger position.
- RLE compression (much longer captures on slow signals over the same USB bandwidth).
- External clock (state analysis) on CH12 (global clock input), clock qualifier.
- Pattern generator / output mode on spare channels (the inputs can be driven through the dividers).
- Frequency counter / timestamps per channel, glitch detection.
- Trace receiver mode (TPIU/PIB, as slogic16u3-trace), protocol offload (UART/SPI decoders in gateware).
- USB serial number from the GW5AT DNA (OTP), multiple units.
