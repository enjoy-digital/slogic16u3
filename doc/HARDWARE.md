# SLogic16U3 Hardware

What is known (and missing) about the Sipeed SLogic16U3 for this project. Sources: the
[slogic16u3-trace](https://github.com/radiolok/slogic16u3-trace) project (USB3/trace pinout, DFU loader
protocol and flash layout), Sipeed's libsigrok driver ([sipeed/libsigrok](https://github.com/sipeed/libsigrok),
`slogic-dev` branch: stock register protocol), Gowin PnR dedicated pins checks (this project).

## FPGA

- Gowin **GW5AT-LV15MG132C1/I0** (GW5AT-15A, MBGA132): 15120 LUT4, 15279 registers, 630Kb BSRAM, one
  GTR12 SerDes quad (Q0, `GTR12_QUADB`/`GTR12_UPARA` primitives), PLLA, OSCA, IDES8/IDES16.
- Gowin IDE: 1.9.10 builds the design (Q0 SerDes configuration from `serdes_toml_to_csr`); it rejects
  initialized BSRAMs on this device (PA2128), slogic16u3-trace uses 1.9.12.04 (device version B).

## Clocks

| Clock      | Source                                        | Notes                                                      |
|------------|-----------------------------------------------|------------------------------------------------------------|
| OSCA       | Internal oscillator (~207MHz / FREQ_DIV).     | ~51.7MHz measured with FREQ_DIV=4 (slogic16u3-trace).      |
| 125MHz XO  | External oscillator, enabled by D14 (=1).     | GTR12 Q0 REFCLK0 (dedicated A8/A7 pads) only.              |

No fabric clock input: accurate fabric clocks have to come from the GTR12 (TX fabric clock, from the
125MHz oscillator).

## USB3

- GTR12 Q0 lane 0: RX A2/A1, TX B3/C3 (dedicated), through a VL162 Type-C mux (orientation handled by
  the mux). 5Gbps only, no USB2 PHY (USB 2.0 hosts/cables: no enumeration).
- C14: USB2 D+ pull-up control, kept at 0. H3/H2 and M2/M1: D+/D- comparators (TLVDS inputs), K3/K2,
  P2/P1: USB2 dx/termination balls (inputs, not driven).

## Inputs

| Channel | Ball | Channel | Ball | Channel | Ball | Channel | Ball |
|---------|------|---------|------|---------|------|---------|------|
| CH0     | J14  | CH4     | N13  | CH8     | N11  | CH12    | N9 (GCLK) |
| CH1     | J13  | CH5     | P13  | CH9     | P11  | CH13    | P9   |
| CH2     | N14  | CH6     | N12  | CH10    | N10  | CH14    | N8   |
| CH3     | P14  | CH7     | P12  | CH11    | P10  | CH15    | P8   |

- ~0.47 input dividers, LVCMOS15UD33 (~0.75V threshold at the pins: ~1.6V at the connector).
- Stock firmware: adjustable threshold 0-6V through a 10-bit DAC (`dac = V/3.33/2*1024`): DAC pin/interface
  not identified yet.
- The channels can be driven as outputs (~1.2V at the connector through the dividers, slogic16u3-trace).

## Other IOs

| Signal      | Ball | Notes                                                    |
|-------------|------|----------------------------------------------------------|
| LED (red)   | G14  | Active high.                                             |
| UART TX/RX  | F14/E14 | On the 8-pin JST SH JTAG connector.                   |
| JTAG        | G2/G3/J2/J3 | Dedicated JTAG pins (FT2232H on the connector).   |
| MODE button | RECONFIG_N (assumed) | Reconfiguration from the multiboot address. |

Gowin PnR dedicated pins (constraining IOs without the `use_*_as_gpio` options):

| Function          | Balls                                  |
|-------------------|----------------------------------------|
| MSPI / CPU-MSPI   | L13 (MSPI), K13, K14, L14, M14, M13    |
| READY / DONE      | M12 / L3                               |
| I2C               | M1                                     |
| SSPI / CPU-SSPI   | H3, H2 / K3, K2, P2, P1                |
| JTAG              | G2, G3, J2, J3                         |

## SPI Flash

- 16MB: application at 0x000000 (loaded at power-up), Sipeed DFU loader (golden image) at 0x800000. The
  GW5AT falls back to the golden image when the application fails its check.
- MSPI pins: K13, K14, L14, L13, M14, M13 (functions to be identified: probed at runtime by the Flash core
  pin mapping, see `slogic_util flash probe`).

## DFU Loader (stock)

- USB `359f:30f1` (MODE button: LED blinks red), USB-to-SPI bridge on Bulk OUT/IN (interface 0), packets
  `[u32 command][u32 length][data]` on ten u32 SPI master registers (ATCSPI200-like): see
  `software/user/libslogic/slogic_dfu.c` (from slogic16u3-trace `tools/slogic_dfu.py` / Sipeed web flasher).

## Stock Firmware (reference)

- USB `359f:3031`, Bulk IN EP 0x82, vendor requests 0x00 (read) / 0x01 (write) on 32-bit registers
  (CTRL 0x04: STOP/RUN/RST, AUX 0x0c: channel mask, samplerate (clock base + divider), threshold DAC,
  test pattern).
- Rates: 5-800MHz (800MHz/N), 4ch@800MHz, 8ch@400MHz, 16ch@200MHz.
- Wire format: sample-major, LSB = CH0, 8/nch samples per byte below 8 channels.
