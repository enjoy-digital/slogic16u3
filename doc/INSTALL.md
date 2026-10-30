# Install the SLogic16U3 LiteX Gateware

This guide installs the open LiteX gateware on a Sipeed SLogic16U3 over USB, with no JTAG and without
opening the case, and gets you back to the stock firmware whenever you want.

> Not affiliated with Sipeed. The tools never write the Sipeed DFU loader, so the MODE button always gets
> you back to it. Take the backup (step 3) before anything else: it is your way back to the stock firmware.

## What You Need

- The SLogic16U3, a **USB3 port and a USB3 cable**: the gateware is SuperSpeed only. On a USB 2.0 port or
  cable it does not enumerate.
- Linux x86_64 for the prebuilt tools (built on Ubuntu 24.04), with libusb-1.0 (`sudo apt install libusb-1.0-0`).
  Other distributions or macOS: build the tools from the sources (`make -C software/user`, needs
  libusb-1.0 headers).
- A release archive `slogic16u3-<version>.tar.gz`: gateware images, tools, udev rules, this guide,
  `BUILDINFO.txt` (sources and toolchain) and `SHA256SUMS`.

## 1. Setup

```sh
tar xf slogic16u3-<version>.tar.gz && cd slogic16u3-<version>
sha256sum -c SHA256SUMS
sudo cp udev/99-slogic16u3.rules /etc/udev/rules.d/ && sudo udevadm control --reload-rules
export PATH=$PWD/tools/linux-x86_64:$PATH
```

The udev rules give your user access to the gateware (`1d50:605a`) and to the Sipeed DFU loader
(`359f:30f1`). Replug the analyzer after installing them.

## 2. Enter the DFU Loader

With the analyzer plugged in, press the **MODE** button (a short press): the LED blinks red and the analyzer
re-enumerates as the Sipeed DFU loader, a USB 2.0 High-Speed device.

> The DFU loader implements USB 2.0 in the FPGA fabric (the GW5AT-15 has no USB 2.0 PHY): on some host ports
> or cables it does not enumerate (`dmesg`: `device descriptor read/64, error -71`). Connect the analyzer
> **through a USB 2.0 hub** for the DFU steps (back to a USB3 port and cable for the gateware itself), or try
> another cable/port. The tools retry the transfers lost on a marginal link.

```sh
lsusb | grep 359f:30f1        # Sipeed DFU loader.
slogic_util dfu id            # Flash JEDEC ID through the loader.
```

The loader is the same with the stock firmware and with this gateware, and nothing here ever writes it.

## 3. Backup (Once, Before Anything Else)

```sh
slogic_util dfu dump slogic16u3-backup.bin
sha256sum slogic16u3-backup.bin > slogic16u3-backup.bin.sha256
```

The dump is the whole 16MB Flash of your unit: the stock application and the DFU loader. The Flash is read
twice and the two reads compared. This takes about 2 minutes, because the loader moves 64 bytes per USB
transfer. Keep the file safe, and keep it private: it contains Sipeed's firmware. The tools check that a
backup belongs to the unit before using it: its DFU loader region must match the Flash.

## 4. Install the Gateware

Still in the DFU loader (MODE button again if you replugged):

```sh
slogic_util dfu update gateware/slogic16u3-<version>.bin --backup slogic16u3-backup.bin
```

The image is checked first: it must be a GW5AT-15 bitstream that fits the application region. Then the
application region (0x000000-0x7fffff) is erased, programmed and verified. Replug the analyzer:

```sh
slogic_util info              # "SLogic16U3 LiteX", 5000 Mb/s, gateware version.
slogic_util check             # Self-test without probes: counter + pattern generator, all modes/rates.
```

## 5. Use It

- `slogic_record`: captures to VCD/CSV/raw (`slogic_record -r 100M -c 16 -n 1M -o capture.vcd`).
- sigrok-cli/PulseView: the `litex-slogic` libsigrok driver (see [SOFTWARE.md](https://github.com/enjoy-digital/slogic_16u3/blob/main/doc/SOFTWARE.md), built from the
  sources with `software/sigrok/build.sh --pulseview`).
- ngscopeclient: `slogic_scopehal_bridge` (see [SOFTWARE.md](https://github.com/enjoy-digital/slogic_16u3/blob/main/doc/SOFTWARE.md)).
- No probes at hand: the pattern generator (`slogic_record -g ...`, sigrok `pattern=generator`) produces UART,
  SPI, PWM, a clock and a Gray counter inside the FPGA, which you can decode in PulseView.

## Update to a New Release

From the running gateware (no MODE button):

```sh
slogic_util flash probe
slogic_util flash update gateware/slogic16u3-<new version>.bin
```

Then replug. Or through the DFU loader, as in step 4.

## Back to the Stock Firmware

```sh
# MODE button -> DFU loader (LED blinking red).
slogic_util dfu restore slogic16u3-backup.bin
```

This writes back the stock application from your backup, checked against the unit first, then verifies it.
Replug: the analyzer runs the Sipeed firmware again (USB `359f:3031`, Sipeed's software and sigrok driver).

## If Something Goes Wrong

| Symptom | What to do |
|---|---|
| An update was interrupted (power loss, unplug). | The FPGA falls back to the DFU loader at power-up (LED blinking red). Run the update or the restore again. |
| `Could not open usb: no device`, `Access denied`. | Check the udev rules (step 1), replug. Right after a re-enumeration, retry after a second. |
| Not enumerated with the gateware. | Use a USB3 port and cable (`lsusb -t`: 5000M). Check that the MODE button was not pressed (DFU loader). |
| `DFU loader ... no device`. | Press MODE (the LED blinks red), check `lsusb` for `359f:30f1`. |
| DFU loader not enumerated (`dmesg`: `error -71`), `I/O error` in DFU commands. | Connect the analyzer through a USB 2.0 hub (or another cable/port) for the DFU steps. |
| `Backup refused: ... another unit?` | The backup is not from this analyzer: use this unit's own dump. |
| `Image refused`. | Use the `.bin` image of a release (the `.fs` file is for JTAG). |

## Flash Layout

| Region | Content | Written by |
|---|---|---|
| 0x000000-0x7fffff | Application: this gateware or the stock firmware. | `dfu update`, `dfu restore`, `flash update`. |
| 0x800000-0xffffff | Sipeed DFU loader (golden image). | Never: refused by the tools. |

More details: [UPDATE.md](https://github.com/enjoy-digital/slogic_16u3/blob/main/doc/UPDATE.md) (update paths and recovery), [HARDWARE.md](https://github.com/enjoy-digital/slogic_16u3/blob/main/doc/HARDWARE.md).
