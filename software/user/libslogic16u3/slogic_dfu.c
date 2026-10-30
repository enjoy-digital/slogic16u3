/* SPDX-License-Identifier: BSD-2-Clause
 *
 * libslogic16u3: Sipeed DFU loader client (USB 359f:30f1, MODE button): configuration SPI Flash access
 * without our gateware (stock units, recovery).
 *
 * The loader exposes a USB-to-SPI bridge on Bulk OUT/IN (interface 0): packets [u32 command][u32 length]
 * [data] (little-endian) on a block of ten u32 SPI master registers (ATCSPI200-like): SET_REGISTER (10
 * registers, then the TX data of the transfer), READ_REGISTER (10 registers back), READ_DATA (RX data of
 * the transfer). Protocol as documented by slogic16u3-trace (tools/slogic_dfu.py, from Sipeed's web
 * flasher), small bridge FIFO: 64-byte reads, 12-byte writes.
 *
 * Writes are restricted to the application region (below 0x800000: the loader itself is never written).
 *
 * This file is part of SLogic16U3.
 *
 * Copyright (c) 2026 Enjoy-Digital <enjoy-digital.fr>
 *
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <libusb.h>

#include "slogic.h"
#include "slogic_dfu.h"
#include "slogic_internal.h"

/* Constants */
/*-----------*/

#define DFU_CMD_SET_REGISTER   0x00020000
#define DFU_CMD_READ_REGISTER  0x00020001
#define DFU_CMD_READ_DATA      0x00020002
#define DFU_REGS               10
#define DFU_TIMEOUT_MS         1000

#define DFU_R_CONTROL          0
#define DFU_R_TRANSFER_FORMAT  1
#define DFU_R_TRANSFER_CONTROL 2
#define DFU_R_TIMING           4
#define DFU_R_CONFIGURATION    6
#define DFU_R_STATUS           7

#define DFU_MODE_WRITE_READ       0x3
#define DFU_MODE_WRITE_ONLY       0x1
#define DFU_MODE_READ_ONLY        0x2
#define DFU_MODE_WRITE_DUMMY_READ 0x5
#define DFU_MODE_NO_DATA          0x7

#define DFU_RETRIES            8  /* Bridge transfer retries (USB errors on marginal High-Speed links). */
#define DFU_READ_CHUNK         64
#define DFU_WRITE_CHUNK        12
#define DFU_BLOCK              65536

struct slogic_dfu {
    libusb_context       *ctx;
    libusb_device_handle *handle;
    uint8_t               ep_out, ep_in;
    uint16_t              mps;
};

/* Bridge */
/*--------*/

static void dfu_default_regs(uint32_t *w)
{
    memset(w, 0, DFU_REGS*sizeof(*w));
    w[DFU_R_TRANSFER_FORMAT] = (0x2 << 16) | (0x7 << 8) | (1 << 7); /* 8-bit, MSB first. */
    w[DFU_R_TIMING]          = (0x2 << 8) | 0xff;
    w[DFU_R_CONFIGURATION]   = (0x6 << 4) | 0x6;
    w[DFU_R_STATUS]          = (1 << 22) | (1 << 14);
}

static uint32_t dfu_transfer_control(unsigned mode, unsigned wr, unsigned rd, unsigned dummy)
{
    return ((mode & 0xf) << 24) | ((wr & 0x1ff) << 12) | ((dummy & 0x3) << 9) | (rd & 0x1ff);
}

static void put32(uint8_t *p, uint32_t v)
{
    p[0] = v; p[1] = v >> 8; p[2] = v >> 16; p[3] = v >> 24;
}

static uint32_t get32(const uint8_t *p)
{
    return p[0] | p[1] << 8 | p[2] << 16 | (uint32_t)p[3] << 24;
}

static int dfu_send(struct slogic_dfu *d, uint32_t cmd, const uint8_t *data, size_t len)
{
    uint8_t pkt[8 + 4*DFU_REGS + 512];
    int n;

    if (len > sizeof(pkt) - 8)
        return SLOGIC_ERR_ARG;
    put32(pkt, cmd);
    put32(pkt + 4, (uint32_t)len);
    if (len)
        memcpy(pkt + 8, data, len);
    if (libusb_bulk_transfer(d->handle, d->ep_out, pkt, (int)(8 + len), &n, DFU_TIMEOUT_MS) ||
        n != (int)(8 + len))
        return SLOGIC_ERR_IO;
    return SLOGIC_OK;
}

static int dfu_recv(struct slogic_dfu *d, uint8_t *data, size_t len)
{
    uint8_t buf[1024];
    size_t size = (len + d->mps - 1)/d->mps*d->mps;
    int n;

    if (size > sizeof(buf))
        return SLOGIC_ERR_ARG;
    if (libusb_bulk_transfer(d->handle, d->ep_in, buf, (int)size, &n, DFU_TIMEOUT_MS) || n < (int)len)
        return SLOGIC_ERR_IO;
    memcpy(data, buf, len);
    return SLOGIC_OK;
}

static int dfu_set_regs(struct slogic_dfu *d, const uint32_t *w, const uint8_t *tx, size_t tx_len)
{
    uint8_t data[4*DFU_REGS + 512];
    for (int i = 0; i < DFU_REGS; i++)
        put32(data + 4*i, w[i]);
    memcpy(data + 4*DFU_REGS, tx, tx_len);
    return dfu_send(d, DFU_CMD_SET_REGISTER, data, 4*DFU_REGS + tx_len);
}

static int dfu_assert_idle(struct slogic_dfu *d)
{
    uint8_t raw[4*DFU_REGS];
    uint32_t status;
    int r;

    if ((r = dfu_send(d, DFU_CMD_READ_REGISTER, NULL, 0)) || (r = dfu_recv(d, raw, sizeof(raw))))
        return r;
    status = get32(raw + 4*DFU_R_STATUS);
    return ((status & 1) || !((status >> 14) & 1)) ? SLOGIC_ERR_IO : SLOGIC_OK;
}

static int dfu_reset(struct slogic_dfu *d)
{
    uint32_t w[DFU_REGS];
    dfu_default_regs(w);
    w[DFU_R_CONTROL]          = 0x7;
    w[DFU_R_TRANSFER_CONTROL] = dfu_transfer_control(DFU_MODE_NO_DATA, 0, 0, 0);
    return dfu_set_regs(d, w, NULL, 0);
}

/* SPI transfer: wr bytes out, then rd bytes in (after dummy bytes). */
static int dfu_xfer_once(struct slogic_dfu *d, const uint8_t *wr, size_t wr_n, uint8_t *rd, size_t rd_n,
                         unsigned dummy)
{
    uint8_t tx[64];
    size_t pad = (4 - wr_n % 4) % 4;
    uint32_t w[DFU_REGS];
    int r;

    if (wr_n + pad > sizeof(tx) || rd_n > 512)
        return SLOGIC_ERR_ARG;
    memcpy(tx, wr, wr_n);
    memset(tx + wr_n, 0xff, pad);
    dfu_default_regs(w);
    if (rd_n == 0)
        w[DFU_R_TRANSFER_CONTROL] = dfu_transfer_control(DFU_MODE_WRITE_ONLY, wr_n - 1, 0, 0);
    else if (wr_n == 0)
        w[DFU_R_TRANSFER_CONTROL] = dfu_transfer_control(DFU_MODE_READ_ONLY, 0, rd_n - 1, 0);
    else if (dummy <= pad)
        w[DFU_R_TRANSFER_CONTROL] = dfu_transfer_control(DFU_MODE_WRITE_READ, wr_n - 1 + dummy, rd_n - 1, 0);
    else
        w[DFU_R_TRANSFER_CONTROL] = dfu_transfer_control(DFU_MODE_WRITE_DUMMY_READ, wr_n - 1 + pad,
                                                         rd_n - 1, dummy - 1 - pad);
    if ((r = dfu_set_regs(d, w, tx, wr_n + pad)))
        return r;
    if (rd_n) {
        if ((r = dfu_send(d, DFU_CMD_READ_DATA, NULL, 0)) || (r = dfu_recv(d, rd, rd_n)))
            return r;
    }
    return dfu_assert_idle(d);
}

/* Bridge recovery after a USB error: endpoints halts cleared, stale IN data drained, bridge reset. */
static void dfu_recover(struct slogic_dfu *d)
{
    uint8_t buf[1024];
    int n;
    libusb_clear_halt(d->handle, d->ep_out);
    libusb_clear_halt(d->handle, d->ep_in);
    while (!libusb_bulk_transfer(d->handle, d->ep_in, buf, sizeof(buf), &n, 20) && n > 0)
        ;
    dfu_reset(d);
}

/* SPI transfer with retries on USB errors (the loader's USB2 is implemented in the FPGA fabric: marginal
   links lose transfers). Retrying is safe: reads, status, write enable/disable, and erase/page program of
   the same data (the updates are verified). */
static int dfu_xfer(struct slogic_dfu *d, const uint8_t *wr, size_t wr_n, uint8_t *rd, size_t rd_n,
                    unsigned dummy)
{
    int r = SLOGIC_ERR_IO;
    for (int i = 0; i < DFU_RETRIES; i++) {
        if ((r = dfu_xfer_once(d, wr, wr_n, rd, rd_n, dummy)) != SLOGIC_ERR_IO)
            return r;
        dfu_recover(d);
    }
    return r;
}

/* Open/Close */
/*------------*/

int slogic_dfu_open(struct slogic_dfu **dp, uint8_t jedec[3])
{
    struct slogic_dfu *d = calloc(1, sizeof(*d));
    struct libusb_config_descriptor *cfg = NULL;
    uint8_t cmd = 0x9f, id[3];
    int r = SLOGIC_ERR_NODEV;

    if (!d)
        return SLOGIC_ERR_NOMEM;
    *dp = NULL;
    if (libusb_init(&d->ctx))
        goto fail;
    d->handle = libusb_open_device_with_vid_pid(d->ctx, SLOGIC_DFU_VID, SLOGIC_DFU_PID);
    if (!d->handle)
        goto fail;
    r = SLOGIC_ERR_IO;
    libusb_set_auto_detach_kernel_driver(d->handle, 1);
    libusb_set_configuration(d->handle, 1);
    if (libusb_claim_interface(d->handle, 0))
        goto fail;
    if (libusb_get_active_config_descriptor(libusb_get_device(d->handle), &cfg))
        goto fail;
    const struct libusb_interface_descriptor *intf = &cfg->interface[0].altsetting[0];
    for (int i = 0; i < intf->bNumEndpoints; i++) {
        const struct libusb_endpoint_descriptor *ep = &intf->endpoint[i];
        if ((ep->bmAttributes & 3) != LIBUSB_TRANSFER_TYPE_BULK)
            continue;
        if (ep->bEndpointAddress & 0x80) {
            d->ep_in = ep->bEndpointAddress;
            d->mps   = ep->wMaxPacketSize ? ep->wMaxPacketSize : 64;
        } else {
            d->ep_out = ep->bEndpointAddress;
        }
    }
    libusb_free_config_descriptor(cfg);
    if (!d->ep_in || !d->ep_out)
        goto fail;
    /* Reset, JEDEC ID (retried once after a reset). */
    for (int i = 0; i < 2; i++) {
        if ((r = dfu_reset(d)))
            goto fail;
        if (!(r = dfu_xfer(d, &cmd, 1, id, 3, 0)) && id[0] != 0x00 && id[0] != 0xff)
            break;
        r = SLOGIC_ERR_FLASH;
    }
    if (r)
        goto fail;
    if (jedec)
        memcpy(jedec, id, 3);
    *dp = d;
    return SLOGIC_OK;
fail:
    slogic_dfu_close(d);
    return r;
}

void slogic_dfu_close(struct slogic_dfu *d)
{
    if (!d)
        return;
    if (d->handle) {
        libusb_release_interface(d->handle, 0);
        libusb_close(d->handle);
    }
    if (d->ctx)
        libusb_exit(d->ctx);
    free(d);
}

/* Flash */
/*-------*/

int slogic_dfu_read(struct slogic_dfu *d, uint32_t addr, uint8_t *data, size_t size,
                    slogic_progress_cb progress, void *user)
{
    size_t total = size, done = 0;
    if (!d || addr + (uint64_t)size > SLOGIC_FLASH_SIZE)
        return SLOGIC_ERR_ARG;
    while (size) {
        size_t n = size > DFU_READ_CHUNK ? DFU_READ_CHUNK : size;
        uint8_t cmd[4] = { 0x0b, addr >> 16, addr >> 8, addr }; /* Fast read (1 dummy byte). */
        int r = dfu_xfer(d, cmd, 4, data, n, 1);
        if (r)
            return r;
        data += n;
        addr += n;
        size -= n;
        done += n;
        if (progress && (done % 65536 == 0 || !size))
            progress("read", done, total, user);
    }
    return SLOGIC_OK;
}

int slogic_dfu_check_backup(struct slogic_dfu *d, const uint8_t *backup, size_t size, char *msg,
                            size_t msg_size)
{
    const uint8_t *gold = backup + SLOGIC_FLASH_APP_SIZE;
    size_t end = SLOGIC_FLASH_SIZE - SLOGIC_FLASH_APP_SIZE;
    uint8_t *flash;
    char tmp[1];
    int r;

    if (!msg) {
        msg      = tmp;
        msg_size = sizeof(tmp);
    }
    if (!backup || size != SLOGIC_FLASH_SIZE) {
        snprintf(msg, msg_size, "backup of %zu bytes: a full Flash dump (%u bytes, dfu dump) is needed", size,
                 SLOGIC_FLASH_SIZE);
        return SLOGIC_ERR_ARG;
    }
    while (end && gold[end - 1] == 0xff)
        end--;
    if (!end) {
        snprintf(msg, msg_size, "no DFU loader at 0x%06x in the backup", SLOGIC_FLASH_APP_SIZE);
        return SLOGIC_ERR_ARG;
    }
    if (!(flash = malloc(end)))
        return SLOGIC_ERR_NOMEM;
    r = slogic_dfu_read(d, SLOGIC_FLASH_APP_SIZE, flash, end, NULL, NULL);
    if (r)
        snprintf(msg, msg_size, "DFU loader region read: %s", slogic_strerror(r));
    else if (memcmp(flash, gold, end)) {
        snprintf(msg, msg_size, "the DFU loader in the Flash differs from the backup's: backup of another unit?");
        r = SLOGIC_ERR_FLASH;
    }
    free(flash);
    return r;
}

static int dfu_wait(struct slogic_dfu *d, double timeout_s)
{
    uint8_t cmd = 0x05, sr;
    double t0 = slogic_time();
    for (;;) {
        int r = dfu_xfer(d, &cmd, 1, &sr, 1, 0);
        if (r)
            return r;
        if (sr == 0xff)
            return SLOGIC_ERR_FLASH;
        if (!(sr & 1))
            return SLOGIC_OK;
        if (slogic_time() - t0 > timeout_s)
            return SLOGIC_ERR_TIMEOUT;
    }
}

static int dfu_write_op(struct slogic_dfu *d, const uint8_t *cmd, size_t n, double timeout_s)
{
    uint8_t wren = 0x06, wrdi = 0x04;
    int r;
    if ((r = dfu_xfer(d, &wren, 1, NULL, 0, 0)))
        return r;
    if ((r = dfu_xfer(d, cmd, n, NULL, 0, 0)) || (r = dfu_wait(d, timeout_s))) {
        dfu_xfer(d, &wrdi, 1, NULL, 0, 0);
        return r;
    }
    return dfu_xfer(d, &wrdi, 1, NULL, 0, 0);
}

int slogic_dfu_update(struct slogic_dfu *d, const uint8_t *image, size_t size,
                      slogic_progress_cb progress, void *user)
{
    size_t blocks = SLOGIC_FLASH_APP_SIZE/DFU_BLOCK; /* The whole application region. */
    uint8_t *check;
    int r;

    if (!d || !image || !size || size > SLOGIC_FLASH_APP_SIZE)
        return SLOGIC_ERR_ARG; /* Never at or above 0x800000 (the loader). */
    for (size_t b = 0; b < blocks; b++) {
        uint32_t a = (uint32_t)(b*DFU_BLOCK);
        uint8_t cmd[4] = { 0xd8, a >> 16, a >> 8, a };
        if ((r = dfu_write_op(d, cmd, 4, 5.0)))
            return r;
        if (progress)
            progress("erase", (b + 1)*DFU_BLOCK, blocks*DFU_BLOCK, user);
    }
    /* Blank check (the image's span). */
    check = malloc(size);
    if (!check)
        return SLOGIC_ERR_NOMEM;
    r = slogic_dfu_read(d, 0, check, size, NULL, NULL);
    for (size_t i = 0; !r && i < size; i++)
        if (check[i] != 0xff)
            r = SLOGIC_ERR_FLASH;
    free(check);
    if (r)
        return r;
    for (size_t o = 0; o < size; ) {
        size_t n = DFU_WRITE_CHUNK;
        if (n > SLOGIC_FLASH_PAGE - (o % SLOGIC_FLASH_PAGE))
            n = SLOGIC_FLASH_PAGE - (o % SLOGIC_FLASH_PAGE);
        if (n > size - o)
            n = size - o;
        int blank = 1;
        for (size_t i = 0; i < n; i++)
            blank &= image[o + i] == 0xff;
        if (!blank) {
            uint8_t cmd[4 + DFU_WRITE_CHUNK] = { 0x02, o >> 16, o >> 8, o };
            memcpy(cmd + 4, image + o, n);
            if ((r = dfu_write_op(d, cmd, 4 + n, 0.1)))
                return r;
        }
        o += n;
        if (progress && (o % 16384 < n || o == size))
            progress("program", o, size, user);
    }
    check = malloc(size);
    if (!check)
        return SLOGIC_ERR_NOMEM;
    r = slogic_dfu_read(d, 0, check, size, progress, user);
    if (!r && memcmp(check, image, size))
        r = SLOGIC_ERR_FLASH;
    free(check);
    if (progress && !r)
        progress("verify", size, size, user);
    return r;
}
