# Host Software

```
  sigrok-cli / PulseView                 ngscopeclient
          │                                    │ TwinLan (SCPI + waveforms)
  libsigrok driver (litex-slogic)       slogic_scopehal_bridge ◄── scopehal driver "slogic"
          │                                    │
          └──────────────► libslogic (C) ◄─────┘      slogic_util / slogic_record
                     │                    │
              USB3 transport        sim transport (software model of the gateware)
          (libusb-1.0: EP0 CSRs,
           Bulk IN transfers ring)
```

Every layer runs on the simulated device (`sim`): the tests, PulseView or ngscopeclient work without hardware.

## libslogic

C library ([`software/user/libslogic`](../software/user/libslogic), BSD-2-Clause):

```c
struct slogic_dev *dev;
slogic_open("usb", &dev);                  /* "usb:serial=0001", "sim", "sim:rate=100e6,fast" */
struct slogic_config cfg = { .samplerate = 200000000, .channels = 16, .limit = 1000000 };
slogic_capture_start(dev, &cfg);
while (slogic_capture_read(dev, &data, &size, 1000) == SLOGIC_OK)
    ...                                    /* Packed samples, see slogic_unpack(). */
slogic_capture_stop(dev, &status);         /* status.overflow: data lost. */
slogic_close(dev);
```

- Configuration checks (`slogic_config_check`: 800MHz/N, channel limits), samplerate lists, unpacking.
- Raw CSR accesses (`slogic_read32/write32/read64/write64/read_burst/write_burst`), sample clock measurement.
- Flash: pads probe, read/erase/program, application update (writes refused at/above 0x800000).
- DFU loader client (Sipeed `359f:30f1`, MODE button): dump/update without our gateware.

Build and test: `make -C software/user && make -C software/user check` (needs libusb-1.0). USB access:
`sudo cp software/udev/99-slogic16u3.rules /etc/udev/rules.d/ && sudo udevadm control --reload-rules`.

## Tools

```sh
slogic_util info                  # Device, gateware, USB3 link, max samplerates.
slogic_util check                 # Self-test: counter pattern captures, all modes/samplerates, data checked.
slogic_util bench 16 200 5        # Throughput: 16 channels at 200MS/s for 5s (overflow reported).
slogic_util clock                 # Sample clock frequency vs host clock (ppm).
slogic_util reg 0x800             # Raw register access.
slogic_util flash probe           # Flash pads mapping + JEDEC ID.
slogic_util flash update app.bin  # Remote update (see UPDATE.md).
slogic_util dfu dump backup.bin   # Full Flash backup from the Sipeed DFU loader (keep it private).

slogic_record -r 100M -c 16 -n 1M -o capture.vcd   # VCD/CSV/u8 (unpacked)/raw packed.
slogic_record -r 800M -c 4 -t 2 -o capture.bin     # Continuous for 2s.
slogic_util -d sim check                           # Any tool on the simulated device.
```

## sigrok / PulseView

Native libsigrok driver `litex-slogic` ([`software/sigrok/litex-slogic`](../software/sigrok/litex-slogic)), on
libslogic: samplerates, sample/time limits, soft triggers with capture ratio, counter test pattern, capture
mode (4/8/16 channels) from the highest enabled channel.

```sh
software/sigrok/build.sh --pulseview        # libsigrok + driver, sigrok-cli, PulseView in software/sigrok/install.
export LD_LIBRARY_PATH=$PWD/software/sigrok/install/lib
software/sigrok/install/bin/sigrok-cli -d litex-slogic --scan
software/sigrok/install/bin/sigrok-cli -d litex-slogic -c samplerate=100M --samples 1M -P uart:rx=CH0 -A uart
software/sigrok/install/bin/sigrok-cli -d litex-slogic:conn=sim -c pattern=counter --samples 16 -O hex
software/sigrok/install/bin/pulseview -d litex-slogic
```

`software/sigrok/litex-slogic.patch`: the driver as a libsigrok patch (upstreaming).

## ngscopeclient

`slogic_scopehal_bridge` exposes the device as a libscopehal remote bridge (SCPI control port 5025, waveform
port 5026), with a software edge trigger and pre-trigger samples:

```sh
software/user/build/slogic_scopehal_bridge            # -d sim for the simulated device.
software/scopehal/install_driver.sh ~/ngscopeclient/lib   # Adds the "slogic" driver, rebuild ngscopeclient.
# ngscopeclient: Add > Oscilloscope > driver "slogic", transport "twinlan", path "localhost:5025:5026".
```

## Python

LiteX tools work directly over USB3 (EP0 CommUSB bridge):

```sh
litex_server --usb --usb-vid 0x1d50 --usb-pid 0x605a
litex_cli --regs                                    # CSRs (build/csr.csv).
```

## Tests

| Test                               | What                                                                    |
|------------------------------------|-------------------------------------------------------------------------|
| `make -C software/user check`      | libslogic on the simulated device: configurations, captures (data checked), overflow, unpacking, Flash probe/update/region guard (175 checks). |
| `test/test_sigrok.py`              | sigrok-cli + litex-slogic: scan, counter pattern captures 4/8/16 channels, invalid configurations. |
| `test/test_scopehal_bridge.py`     | Bridge protocol as libscopehal drives it: identification, free running and triggered waveforms. |
| `test/test_regs.py`                | slogic_regs.h against the target csr.csv.                                |
