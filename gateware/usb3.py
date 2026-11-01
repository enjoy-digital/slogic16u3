#
# This file is part of SLogic16U3.
#
# Copyright (c) 2026 Florent Kermarrec <florent@enjoy-digital.fr>
# SPDX-License-Identifier: BSD-2-Clause

"""USB3 Device: USB3-PIPE Device Core on a USB3 PHY.

- EP0: CSRs/SoC bus accesses over vendor requests (USB3EP0Wishbone, LiteX CommUSB protocol:
  `litex_server --usb`, libliteusb).
- EP1 Bulk IN: capture data (sink: data_width-bit words, first byte on data[0:8], `last`: end of the
  capture, short packet).

https://github.com/enjoy-digital/usb3_pipe
"""

from migen import *

from litex.gen import *

from litex.soc.interconnect.csr import *
from litex.soc.interconnect     import stream
from litex.soc.interconnect     import wishbone

# Constants ----------------------------------------------------------------------------------------

USB3_VID       = 0x1d50
USB3_PID       = 0x605a
USB3_DATA_EPT  = 1 # Bulk IN (Capture -> Host).
USB3_MAX_BURST = 16 # Host windows of Max Burst packets (ex: AMD xHCI): 8 caps Bulk IN at ~370MB/s.

# USB3 Device --------------------------------------------------------------------------------------

class USB3Device(LiteXModule):
    def __init__(self, phy, sys_clk_freq, data_width=64,
        vid          = USB3_VID,
        pid          = USB3_PID,
        manufacturer = "Enjoy-Digital",
        product      = "SLogic16U3 LiteX",
        serial       = "0001",
        max_burst    = USB3_MAX_BURST):
        from usb3_core.device      import USB3DeviceCore
        from usb3_core.bulk        import USB3BulkIN
        from usb3_core.descriptors import USB3Descriptors, USB3InterfaceDescriptor, USB3EndpointDescriptor
        from usb3_core.wishbone    import USB3EP0Wishbone

        self.sink  = sink = stream.Endpoint([("data", data_width)])
        self.flush = Signal() # i: Drop the buffered data (capture restarted).

        # # #

        # Descriptors: vendor interface with a Bulk IN endpoint.
        descriptors = USB3Descriptors(
            interfaces   = [USB3InterfaceDescriptor(0, endpoints=[
                USB3EndpointDescriptor(USB3_DATA_EPT, direction=1, max_burst=max_burst),
            ])],
            vid          = vid,
            pid          = pid,
            manufacturer = manufacturer,
            product      = product,
            serial       = serial,
        )

        # Device Core ------------------------------------------------------------------------------
        self.bulk_in = bulk_in = USB3BulkIN(ept=USB3_DATA_EPT, max_burst=max_burst, data_width=data_width)
        # CRC-32s from chained 1-byte stages: less logic (GW5AT-15 nearly full), sys at 62.5MHz.
        self.core    = core    = USB3DeviceCore(phy, sys_clk_freq, descriptors, [bulk_in],
            with_ep0_handler = True,
            crc32_cascade    = True)

        # EP0 Wishbone Bridge ----------------------------------------------------------------------
        # Registered to the SoC bus (timings: the bridge address/data come from its FIFO memory, the
        # CSR decoders/read multiplexers behind): +1 cycle per access, strobe dropped on ack (no
        # repeated access).
        self.wishbone = USB3EP0Wishbone(core.ep0)
        self.bus = bus = wishbone.Interface(data_width=32, address_width=32, addressing="word")
        master = self.wishbone.bus
        self.sync += [
            bus.adr.eq(master.adr),
            bus.dat_w.eq(master.dat_w),
            bus.sel.eq(master.sel),
            bus.we.eq(master.we),
            bus.cyc.eq(master.cyc & ~bus.ack),
            bus.stb.eq(master.stb & master.cyc & ~bus.ack),
        ]
        self.comb += [
            master.ack.eq(bus.ack),
            master.err.eq(bus.err),
            master.dat_r.eq(bus.dat_r),
        ]

        # Bulk IN ----------------------------------------------------------------------------------
        self.comb += [
            sink.connect(bulk_in.sink, omit={"be", "first"}),
            bulk_in.sink.be.eq(2**(data_width//8) - 1),
            bulk_in.flush.eq(self.flush),
        ]
