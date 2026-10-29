/* SPDX-License-Identifier: BSD-2-Clause
 *
 * libslogic: USB3 transport (libusb-1.0).
 *
 * - EP0: SoC bus accesses with vendor requests (LiteX CommUSB protocol, USB3-PIPE USB3EP0Wishbone):
 *   bmRequestType 0xc3 (read)/0x43 (write), bRequest 0, wValue/wIndex: address (low/high 16-bit), up to
 *   128 words per transfer.
 * - EP1 Bulk IN: capture data, ring of asynchronous transfers (completed in order), a short transfer
 *   ends the capture (gateware `last`: short packet).
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
#include "slogic_internal.h"

/* Constants */
/*-----------*/

#define SLOGIC_USB_VID          0x1d50
#define SLOGIC_USB_PID          0x605a
#define SLOGIC_USB_PRODUCT      "SLogic16U3 LiteX"
#define SLOGIC_USB_INTERFACE    0
#define SLOGIC_USB_DATA_EP      0x81
#define SLOGIC_USB_WB_READ      0xc3
#define SLOGIC_USB_WB_WRITE     0x43
#define SLOGIC_USB_WB_REQUEST   0x00
#define SLOGIC_USB_WB_MAX_WORDS 128
#define SLOGIC_USB_CTRL_TIMEOUT 1000

#define SLOGIC_USB_XFERS        16
#define SLOGIC_USB_XFER_SIZE    (256*1024)

/* Context */
/*---------*/

struct slogic_usb_xfer {
    struct libusb_transfer *transfer;
    uint8_t *buf;
    int      done;
    int      status;
    int      length;
};

struct slogic_usb {
    libusb_context       *ctx;
    libusb_device_handle *handle;
    int                   claimed;
    int                   speed;
    char                  serial[64];
    /* Stream. */
    struct slogic_usb_xfer xfers[SLOGIC_USB_XFERS];
    int                    streaming;
    int                    head;    /* Next transfer to return. */
    int                    pending; /* Transfer returned by the last read (resubmitted next). */
    int                    ended;
};

/* Bus */
/*-----*/

static int slogic_usb_read_burst(void *ctx, uint32_t addr, uint32_t *vals, size_t count)
{
    struct slogic_usb *u = ctx;
    uint8_t data[4*SLOGIC_USB_WB_MAX_WORDS];

    while (count) {
        size_t n = count > SLOGIC_USB_WB_MAX_WORDS ? SLOGIC_USB_WB_MAX_WORDS : count;
        int rc = libusb_control_transfer(u->handle, SLOGIC_USB_WB_READ, SLOGIC_USB_WB_REQUEST,
            addr & 0xffff, addr >> 16, data, (uint16_t)(4*n), SLOGIC_USB_CTRL_TIMEOUT);
        if (rc != (int)(4*n))
            return SLOGIC_ERR_IO;
        for (size_t i = 0; i < n; i++)
            vals[i] = (uint32_t)data[4*i] | (uint32_t)data[4*i + 1] << 8 |
                      (uint32_t)data[4*i + 2] << 16 | (uint32_t)data[4*i + 3] << 24;
        vals  += n;
        addr  += 4*n;
        count -= n;
    }
    return SLOGIC_OK;
}

static int slogic_usb_write_burst(void *ctx, uint32_t addr, const uint32_t *vals, size_t count)
{
    struct slogic_usb *u = ctx;
    uint8_t data[4*SLOGIC_USB_WB_MAX_WORDS];

    while (count) {
        size_t n = count > SLOGIC_USB_WB_MAX_WORDS ? SLOGIC_USB_WB_MAX_WORDS : count;
        for (size_t i = 0; i < n; i++) {
            data[4*i + 0] = vals[i] >>  0;
            data[4*i + 1] = vals[i] >>  8;
            data[4*i + 2] = vals[i] >> 16;
            data[4*i + 3] = vals[i] >> 24;
        }
        int rc = libusb_control_transfer(u->handle, SLOGIC_USB_WB_WRITE, SLOGIC_USB_WB_REQUEST,
            addr & 0xffff, addr >> 16, data, (uint16_t)(4*n), SLOGIC_USB_CTRL_TIMEOUT);
        if (rc != (int)(4*n))
            return SLOGIC_ERR_IO;
        vals  += n;
        addr  += 4*n;
        count -= n;
    }
    return SLOGIC_OK;
}

/* Stream */
/*--------*/

static void LIBUSB_CALL slogic_usb_callback(struct libusb_transfer *transfer)
{
    struct slogic_usb_xfer *x = transfer->user_data;
    x->status = transfer->status;
    x->length = transfer->actual_length;
    x->done   = 1;
}

static int slogic_usb_submit(struct slogic_usb *u, int i)
{
    struct slogic_usb_xfer *x = &u->xfers[i];
    x->done = 0;
    libusb_fill_bulk_transfer(x->transfer, u->handle, SLOGIC_USB_DATA_EP, x->buf,
        SLOGIC_USB_XFER_SIZE, slogic_usb_callback, x, 0);
    return libusb_submit_transfer(x->transfer) ? SLOGIC_ERR_IO : SLOGIC_OK;
}

static int slogic_usb_stream_stop(void *ctx)
{
    struct slogic_usb *u = ctx;

    if (!u->streaming)
        return SLOGIC_OK;
    for (int i = 0; i < SLOGIC_USB_XFERS; i++)
        if (!u->xfers[i].done)
            libusb_cancel_transfer(u->xfers[i].transfer);
    /* Wait for the cancellations (bounded). */
    double t0 = slogic_time();
    for (;;) {
        int busy = 0;
        for (int i = 0; i < SLOGIC_USB_XFERS; i++)
            busy |= !u->xfers[i].done;
        if (!busy || slogic_time() - t0 > 1.0)
            break;
        struct timeval tv = { 0, 10000 };
        libusb_handle_events_timeout_completed(u->ctx, &tv, NULL);
    }
    u->streaming = 0;
    return SLOGIC_OK;
}

static int slogic_usb_stream_start(void *ctx)
{
    struct slogic_usb *u = ctx;

    slogic_usb_stream_stop(u);
    /* Endpoint halt cleared: Sequence Numbers reset (the gateware flushes its stale data on start). */
    libusb_clear_halt(u->handle, SLOGIC_USB_DATA_EP);
    for (int i = 0; i < SLOGIC_USB_XFERS; i++) {
        if (!u->xfers[i].transfer) {
            u->xfers[i].transfer = libusb_alloc_transfer(0);
            u->xfers[i].buf      = malloc(SLOGIC_USB_XFER_SIZE);
            if (!u->xfers[i].transfer || !u->xfers[i].buf)
                return SLOGIC_ERR_NOMEM;
        }
        u->xfers[i].done = 1;
    }
    for (int i = 0; i < SLOGIC_USB_XFERS; i++)
        if (slogic_usb_submit(u, i))
            return SLOGIC_ERR_IO;
    u->head      = 0;
    u->pending   = -1;
    u->ended     = 0;
    u->streaming = 1;
    return SLOGIC_OK;
}

static int slogic_usb_stream_read(void *ctx, const uint8_t **data, size_t *size, int *last, int timeout_ms)
{
    struct slogic_usb *u = ctx;
    struct slogic_usb_xfer *x;
    double t0 = slogic_time();

    if (!u->streaming || u->ended)
        return SLOGIC_ERR_END;
    /* Previous buffer released: resubmitted. */
    if (u->pending >= 0) {
        if (slogic_usb_submit(u, u->pending))
            return SLOGIC_ERR_IO;
        u->pending = -1;
    }
    x = &u->xfers[u->head];
    while (!x->done) {
        int remaining = timeout_ms - (int)((slogic_time() - t0)*1e3);
        if (timeout_ms >= 0 && remaining <= 0)
            return SLOGIC_ERR_TIMEOUT;
        struct timeval tv = { 0, 100000 };
        if (timeout_ms >= 0 && remaining < 100)
            tv.tv_usec = remaining*1000;
        if (libusb_handle_events_timeout_completed(u->ctx, &tv, &x->done) &&
            !x->done)
            return SLOGIC_ERR_IO;
    }
    if (x->status != LIBUSB_TRANSFER_COMPLETED)
        return SLOGIC_ERR_IO;
    *data = x->buf;
    *size = x->length;
    *last = x->length < SLOGIC_USB_XFER_SIZE;
    if (*last)
        u->ended = 1;
    else
        u->pending = u->head;
    u->head = (u->head + 1) % SLOGIC_USB_XFERS;
    return SLOGIC_OK;
}

/* Open/Close */
/*------------*/

static void slogic_usb_close(void *ctx)
{
    struct slogic_usb *u = ctx;

    if (!u)
        return;
    slogic_usb_stream_stop(u);
    for (int i = 0; i < SLOGIC_USB_XFERS; i++) {
        if (u->xfers[i].transfer)
            libusb_free_transfer(u->xfers[i].transfer);
        free(u->xfers[i].buf);
    }
    if (u->claimed)
        libusb_release_interface(u->handle, SLOGIC_USB_INTERFACE);
    if (u->handle)
        libusb_close(u->handle);
    if (u->ctx)
        libusb_exit(u->ctx);
    free(u);
}

static void slogic_usb_get_info(void *ctx, struct slogic_info *info)
{
    struct slogic_usb *u = ctx;
    snprintf(info->transport, sizeof(info->transport), "usb");
    snprintf(info->serial, sizeof(info->serial), "%s", u->serial);
    info->usb_speed = u->speed;
}

static const struct slogic_transport slogic_usb_transport = {
    .name         = "usb",
    .read_burst   = slogic_usb_read_burst,
    .write_burst  = slogic_usb_write_burst,
    .stream_start = slogic_usb_stream_start,
    .stream_read  = slogic_usb_stream_read,
    .stream_stop  = slogic_usb_stream_stop,
    .close        = slogic_usb_close,
    .get_info     = slogic_usb_get_info,
};

static void slogic_usb_arg(const char *args, const char *key, char *val, size_t size)
{
    const char *p = strstr(args, key);
    size_t n = 0;
    val[0] = '\0';
    if (!p || p[strlen(key)] != '=')
        return;
    p += strlen(key) + 1;
    while (p[n] && p[n] != ',' && n < size - 1)
        n++;
    memcpy(val, p, n);
    val[n] = '\0';
}

int slogic_usb_open(const char *args, struct slogic_dev *dev)
{
    struct slogic_usb *u;
    libusb_device **list = NULL;
    char want_serial[64], want_path[32];
    ssize_t n;

    slogic_usb_arg(args, "serial", want_serial, sizeof(want_serial));
    slogic_usb_arg(args, "path",   want_path,   sizeof(want_path));

    u = calloc(1, sizeof(*u));
    if (!u)
        return SLOGIC_ERR_NOMEM;
    if (libusb_init(&u->ctx)) {
        free(u);
        return SLOGIC_ERR_IO;
    }
    n = libusb_get_device_list(u->ctx, &list);
    for (ssize_t i = 0; i < n && !u->handle; i++) {
        struct libusb_device_descriptor desc;
        libusb_device_handle *h;
        char product[64] = "", serial[64] = "", path[32];
        uint8_t ports[8];
        int np, len;

        if (libusb_get_device_descriptor(list[i], &desc) || desc.idVendor != SLOGIC_USB_VID ||
            desc.idProduct != SLOGIC_USB_PID)
            continue;
        np  = libusb_get_port_numbers(list[i], ports, sizeof(ports));
        len = snprintf(path, sizeof(path), "%u", libusb_get_bus_number(list[i]));
        for (int k = 0; k < np && len < (int)sizeof(path); k++)
            len += snprintf(path + len, sizeof(path) - len, "%c%u", k ? '.' : '-', ports[k]);
        if (want_path[0] && strcmp(want_path, path))
            continue;
        if (libusb_open(list[i], &h))
            continue;
        if (desc.iProduct)
            libusb_get_string_descriptor_ascii(h, desc.iProduct, (unsigned char *)product, sizeof(product));
        if (desc.iSerialNumber)
            libusb_get_string_descriptor_ascii(h, desc.iSerialNumber, (unsigned char *)serial, sizeof(serial));
        /* Other USB3-PIPE devices may share the VID/PID: product string checked. */
        if (strcmp(product, SLOGIC_USB_PRODUCT) || (want_serial[0] && strcmp(want_serial, serial))) {
            libusb_close(h);
            continue;
        }
        u->handle = h;
        u->speed  = libusb_get_device_speed(list[i]) == LIBUSB_SPEED_SUPER ? 5000 :
                    libusb_get_device_speed(list[i]) == LIBUSB_SPEED_SUPER_PLUS ? 10000 : 0;
        snprintf(u->serial, sizeof(u->serial), "%s", serial);
    }
    if (list)
        libusb_free_device_list(list, 1);
    if (!u->handle) {
        slogic_usb_close(u);
        return SLOGIC_ERR_NODEV;
    }
    libusb_set_auto_detach_kernel_driver(u->handle, 1);
    if (libusb_claim_interface(u->handle, SLOGIC_USB_INTERFACE)) {
        slogic_usb_close(u);
        return SLOGIC_ERR_IO;
    }
    u->claimed = 1;
    dev->t   = &slogic_usb_transport;
    dev->ctx = u;
    return SLOGIC_OK;
}
