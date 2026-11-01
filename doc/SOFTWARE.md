# Host Software

```
  sigrok-cli / PulseView                 ngscopeclient
          │                                    │ TwinLan (SCPI + waveforms)
  libsigrok driver (litex-slogic)       slogic16u3_scopehal_bridge ◄── scopehal driver "slogic"
          │                                    │
          └──────────────► libslogic16u3 (C) ◄─────┘      slogic16u3_util / slogic16u3_record
                     │                    │
              USB3 transport        sim transport (software model of the gateware)
          (libusb-1.0: EP0 CSRs,
           Bulk IN transfers ring)
```

Every layer runs on the simulated device (`sim`): the tests, PulseView or ngscopeclient work without hardware.

## libslogic16u3

C library ([`software/user/libslogic16u3`](../software/user/libslogic16u3), BSD-2-Clause):

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

Hardware trigger (gateware 0.2): nothing is streamed until the trigger, then the pre-trigger samples kept
by the gateware come first:

```c
struct slogic_config cfg = { .samplerate = 200000000, .channels = 16, .limit = 100000, /* After the trigger. */
                             .trigger_enable = 1, .pretrigger = 4000 };     /* <= slogic_max_pretrigger(). */
slogic_trigger_parse("CH1=f,CH6=1", &cfg.trigger);  /* Or .zero/.one/.rising/.falling channel masks. */
slogic_capture_start(dev, &cfg);
/* slogic_capture_read(): SLOGIC_ERR_TIMEOUT while waiting for the trigger. Once data is received,
   slogic_capture_status(): status.trigger_offset = trigger sample index in the stream (>= pretrigger
   unless the trigger came earlier), status.trigger_sample = index since the capture start. */
```

Max pre-trigger: 7680/15360/30720 samples with 16/8/4 channels (the capture FIFO, doc/GATEWARE.md).

Outputs (gateware 0.3): AUX0/AUX1, plus the output channels of the build. They are tristate until enabled:

```c
int out = slogic_output_find(&info, "AUX0");             /* slogic_output_count/name/channel. */
struct slogic_output o = { .enable = 1, .source = SLOGIC_OUT_TRIGGER };
slogic_output_timing(dev, 10, 0, 1, 0);                  /* Pulse width 10 cycles, delay 0 (100MHz cycles). */
slogic_output_set(dev, out, &o);                         /* Trigger out from the next triggered capture. */
slogic_outputs_off(dev);                                  /* All tristate. */
```

- Configuration checks (`slogic_config_check`: 800MHz/N, channel limits, trigger), samplerate lists, unpacking.
- Raw CSR accesses (`slogic_read32/write32/read64/write64/read_burst/write_burst`), sample clock measurement.
- Flash: pads probe, read/erase/program, application update (writes refused at/above 0x800000).
- DFU loader client (Sipeed `359f:30f1`, MODE button): dump/update without our gateware.

Build and test: `make -C software/user && make -C software/user check` (needs libusb-1.0). USB access:
`sudo cp software/udev/99-slogic16u3.rules /etc/udev/rules.d/ && sudo udevadm control --reload-rules`.

## Tools

```sh
slogic16u3_util info                  # Device, gateware, USB3 link, max samplerates.
slogic16u3_util check                 # Self-test: counter + pattern generator captures, all modes/samplerates,
                                  # hardware trigger (pattern generator: positions, windows), every sample
                                  # checked (check [samples] counter|generator|trigger|all).
slogic16u3_util bench 16 200 5        # Throughput: 16 channels at 200MS/s for 5s (overflow reported).
slogic16u3_util clock                 # Sample clock frequency vs host clock (ppm).
slogic16u3_util reg 0x800             # Raw register access.
slogic16u3_util out                   # Outputs: state, pad levels (AUX0/AUX1, output channels).
slogic16u3_util out AUX0 trigger      # Trigger out (out timing <width> <delay> <period> <high>: cycles).
slogic16u3_util out AUX1 periodic     # Reference ticks / square wave (period/high from out timing).
slogic16u3_util out all off           # Back to tristate.
slogic16u3_util loopback AUX0 CH0     # AUX0 wired to CH0: generator checked through the real input path.
slogic16u3_util flash probe           # Flash pads mapping + JEDEC ID.
slogic16u3_util flash update app.bin  # Remote update (see UPDATE.md).
slogic16u3_util dfu dump backup.bin   # Full Flash backup from the Sipeed DFU loader (keep it private).

slogic16u3_record -r 100M -c 16 -n 1M -o capture.vcd   # VCD/CSV/u8 (unpacked)/raw packed.
slogic16u3_record -r 800M -c 4 -t 2 -o capture.bin     # Continuous for 2s.
slogic16u3_record -g -r 100M -c 16 -n 1M -o gen.vcd    # Pattern generator (no probes): UART/SPI/PWM/clock/Gray.
slogic16u3_record -r 200M -c 16 -T CH1=f -P 4000 -n 100k -o trig.vcd # Hardware trigger, 4000 pre-trigger samples.
slogic16u3_util -d sim check                           # Any tool on the simulated device.
```

## sigrok / PulseView

Native libsigrok driver `litex-slogic` ([`software/sigrok/litex-slogic`](../software/sigrok/litex-slogic)), on
libslogic16u3: samplerates, sample/time limits, triggers with capture ratio, test patterns (counter, generator), capture
mode (4/8/16 channels) from the highest enabled channel. Triggers: hardware trigger (gateware 0.2) when it can
(single stage, pre-trigger samples = capture ratio x samples within the gateware buffer), else the libsigrok
soft trigger (multi-stage, deeper pre-trigger: the host processes the whole stream).

```sh
software/sigrok/build.sh --pulseview        # libsigrok + driver, sigrok-cli, PulseView in software/sigrok/install.
export LD_LIBRARY_PATH=$PWD/software/sigrok/install/lib
software/sigrok/install/bin/sigrok-cli -d litex-slogic --scan
software/sigrok/install/bin/sigrok-cli -d litex-slogic -c samplerate=100M --samples 1M -P uart:rx=CH0 -A uart
software/sigrok/install/bin/sigrok-cli -d litex-slogic:conn=sim -c pattern=counter --samples 16 -O hex
software/sigrok/install/bin/sigrok-cli -d litex-slogic -c samplerate=100M:pattern=generator --samples 200k \
    -P uart:rx=CH0:baudrate=1000000 -A uart=rx-data     # Pattern generator: the UART text, no probes.
software/sigrok/install/bin/sigrok-cli -d litex-slogic -c samplerate=200M:captureratio=10 --samples 100k \
    --triggers CH1=f --wait-trigger -O vcd -o trig.vcd  # Hardware trigger (-l 4: "Hardware trigger" logged).
software/sigrok/install/bin/pulseview -d litex-slogic
```

`software/sigrok/litex-slogic.patch`: the driver as a libsigrok patch (upstreaming).

## ngscopeclient

`slogic16u3_scopehal_bridge` exposes the device as a libscopehal remote bridge (SCPI control port 5025, waveform
port 5026), with an edge trigger and pre-trigger samples (hardware trigger with gateware 0.2 when the
pre-trigger fits, else software):

```sh
software/user/build/slogic16u3_scopehal_bridge            # -d sim for the simulated device.
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
| `make -C software/user check`      | libslogic16u3 on the simulated device: configurations, captures (data checked), overflow, unpacking, Flash probe/update/region guard, pattern generator decoded (UART/SPI/clock/Gray), hardware trigger (positions, windows, checks), outputs (discovery, set/get, readback captures). |
| `test/test_sigrok.py`              | sigrok-cli + litex-slogic: scan, counter pattern captures 4/8/16 channels, pattern generator UART/SPI decoded, invalid configurations, hardware trigger and soft trigger fallback. |
| `test/test_scopehal_bridge.py`     | Bridge protocol as libscopehal drives it: identification, free running and triggered waveforms. |
| `test/test_regs.py`                | slogic_regs.h against the target csr.csv.                                |
