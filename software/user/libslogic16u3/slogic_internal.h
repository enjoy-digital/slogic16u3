/* SPDX-License-Identifier: BSD-2-Clause
 *
 * libslogic16u3 internals: transports (USB, simulated device).
 *
 * This file is part of SLogic16U3.
 *
 * Copyright (c) 2026 Enjoy-Digital <enjoy-digital.fr>
 *
 */

#ifndef SLOGIC_INTERNAL_H
#define SLOGIC_INTERNAL_H

#include "slogic.h"

/* Transport */
/*-----------*/

struct slogic_transport {
    const char *name;
    /* SoC bus (byte addresses, 32-bit words). */
    int  (*read_burst)(void *ctx, uint32_t addr, uint32_t *vals, size_t count);
    int  (*write_burst)(void *ctx, uint32_t addr, const uint32_t *vals, size_t count);
    /* Capture data stream (Bulk IN): start (stale data flushed), next buffer, stop. A short buffer
     * (size < requested) ends the stream (gateware `last`: capture done). */
    int  (*stream_start)(void *ctx);
    int  (*stream_read)(void *ctx, const uint8_t **data, size_t *size, int *last, int timeout_ms);
    int  (*stream_stop)(void *ctx);
    void (*close)(void *ctx);
    /* Info. */
    void (*get_info)(void *ctx, struct slogic_info *info);
};

struct slogic_dev {
    const struct slogic_transport *t;
    void *ctx;
    struct slogic_info info;
    /* Capture state. */
    struct slogic_config config;
    int      capturing;
    int      ended;
    uint64_t bytes;
    uint64_t bytes_limit;   /* Packed bytes of the capture (limit), 0: continuous. */
    /* Flash state. */
    int      flash_open;
    uint32_t flash_control;
};

/* Transports */
int slogic_usb_open(const char *args, struct slogic_dev *dev);
int slogic_sim_open(const char *args, struct slogic_dev *dev);

void slogic_sleep_ms(unsigned ms);

#endif /* SLOGIC_INTERNAL_H */
