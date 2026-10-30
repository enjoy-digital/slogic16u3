# Update and Recovery

The SLogic16U3 can't be bricked from USB as long as the Sipeed DFU loader is kept: it is the golden image of the
GW5AT multiboot and nothing in this project ever writes it.

## Flash Layout (16MB)

| Region            | Content                                              | Written by                                   |
|-------------------|------------------------------------------------------|----------------------------------------------|
| 0x000000-0x7fffff | Application bitstream (this gateware or the stock one). | `slogic_util flash update`, `slogic_util dfu update/restore` |
| 0x800000-0xffffff | Sipeed DFU loader (golden image).                    | Never (refused by libslogic, both paths).    |

- At power-up the GW5AT loads the application at 0x000000, a corrupted application fails its check and the
  FPGA falls back to the golden image (DFU loader).
- Our bitstream is built with multiboot to 0x800000: the **MODE button** (RECONFIG_N) reloads the DFU loader at
  any time (LED blinking red, USB `359f:30f1`).

## First Install (from the stock firmware)

User guide with a release package: [INSTALL.md](INSTALL.md).

```sh
# MODE button -> DFU loader (LED blinks red).
slogic_util dfu dump slogic16u3-backup.bin       # Full 16MB backup (read twice): keep it private, outside the repository.
slogic_util dfu update build/gateware/sipeed_slogic16u3.bin --backup slogic16u3-backup.bin
# Replug: the analyzer enumerates as "SLogic16U3 LiteX" (1d50:605a, 5Gbps).
slogic_util info
```

Safety checks (libslogic, both update paths): the image must be a GW5AT-15 bitstream (sync word and IDCODE
in its header) that fits the application region (`slogic_image_check`); the DFU path needs the full dump of
the unit, checked against the Flash's DFU loader region (`slogic_dfu_check_backup`), and erases the whole
application region before programming (no tail of the previous image left), then verifies.

## Remote Update (from our gateware)

```sh
slogic_util flash probe                          # Flash pads mapping + JEDEC ID (first time: MSPI pads probed).
slogic_util flash update build/gateware/sipeed_slogic16u3.bin   # Erase, program, verify (0x000000 only).
# Replug to boot the new gateware.
```

The update goes through the gateware Flash core (EP0 CSRs + 512-byte buffers, SPI at ~10MHz): a page program per
EP0 burst. If anything goes wrong (power loss, bad image), the GW5AT boots the DFU loader: update again with
`slogic_util dfu update`.

## Back to the Stock Firmware

```sh
# MODE button -> DFU loader.
slogic_util dfu restore slogic16u3-backup.bin      # The backup's application region (checked, verified).
```

## Release

```sh
./scripts/build_best.py                           # Bitstream (timings met) in build/.
./scripts/release.py 0.1.0                        # release/slogic16u3-0.1.0[.tar.gz]: gateware .bin/.fs,
                                                  # Linux tools, udev rules, INSTALL.md, BUILDINFO, SHA256SUMS.
```

## Open Points

- The MSPI pads functions (K13/K14/L14/L13/M14/M13) are probed at runtime: once confirmed on hardware, the
  mapping becomes the Flash core default (and a LiteX-Boards `spiflash` resource).
- Reconfiguration from the gateware (reboot after an update without replug) is not available yet (GW5AT
  `SAMB` + reconfiguration trigger to investigate): replug or MODE + replug.
