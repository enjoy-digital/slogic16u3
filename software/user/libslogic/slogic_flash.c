/* SPDX-License-Identifier: BSD-2-Clause
 *
 * libslogic: configuration SPI Flash access through the gateware Flash core (remote update).
 *
 * SPI transfers: bytes written to the TX buffer, transfer started, received bytes read from the RX buffer
 * (same offsets, up to SLOGIC_FLASH_BUFFER_SIZE bytes per transfer, CS_N held between transfers with
 * hold). The MSPI pads functions are probed: JEDEC ID read with each CLK/CS_N/MOSI/MISO mapping, confirmed
 * by the SFDP signature.
 *
 * Writes (erase/program) are restricted to the application region (below 0x800000): the Sipeed DFU
 * loader (golden image, recovery) is never written.
 *
 * This file is part of SLogic16U3.
 *
 * Copyright (c) 2026 Enjoy-Digital <enjoy-digital.fr>
 *
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "slogic.h"
#include "slogic_internal.h"
#include "slogic_regs.h"

/* SPI Flash commands */
#define FLASH_CMD_WREN      0x06
#define FLASH_CMD_RDSR      0x05
#define FLASH_CMD_READ      0x03
#define FLASH_CMD_PP        0x02
#define FLASH_CMD_SE        0x20 /* 4KB. */
#define FLASH_CMD_BE64      0xd8 /* 64KB. */
#define FLASH_CMD_RDID      0x9f
#define FLASH_CMD_RDSFDP    0x5a

#define FLASH_DIVIDER       2    /* SPI clock = sys/6 (~10MHz). */

/* Transfers */
/*-----------*/

static int flash_xfer(struct slogic_dev *dev, const uint8_t *tx, uint8_t *rx, size_t len, int hold)
{
    uint32_t words[SLOGIC_FLASH_BUFFER_SIZE/4];
    size_t nwords = (len + 3)/4;
    uint32_t status;
    int r;

    if (len == 0 || len > SLOGIC_FLASH_BUFFER_SIZE)
        return SLOGIC_ERR_ARG;
    memset(words, 0, sizeof(words));
    for (size_t i = 0; i < len; i++)
        words[i/4] |= (uint32_t)(tx ? tx[i] : 0) << (8*(i % 4));
    if ((r = slogic_write_burst(dev, SLOGIC_FLASH_BUFFER_BASE, words, nwords)))
        return r;
    if ((r = slogic_write32(dev, SLOGIC_FLASH_XFER, (uint32_t)len | (hold ? SLOGIC_FLASH_XFER_HOLD : 0))))
        return r;
    if ((r = slogic_write32(dev, SLOGIC_FLASH_CTRL, SLOGIC_FLASH_CTRL_START)))
        return r;
    for (int i = 0; ; i++) {
        if ((r = slogic_read32(dev, SLOGIC_FLASH_STATUS, &status)))
            return r;
        if (!(status & SLOGIC_FLASH_STATUS_BUSY))
            break;
        if (i > 1000)
            return SLOGIC_ERR_TIMEOUT;
    }
    if (!rx)
        return SLOGIC_OK;
    if ((r = slogic_read_burst(dev, SLOGIC_FLASH_BUFFER_BASE, words, nwords)))
        return r;
    for (size_t i = 0; i < len; i++)
        rx[i] = (uint8_t)(words[i/4] >> (8*(i % 4)));
    return SLOGIC_OK;
}

static int flash_set_pins(struct slogic_dev *dev, const struct slogic_flash_pins *p)
{
    int r;
    dev->flash_control = SLOGIC_FLASH_CONTROL_ENABLE |
                         SLOGIC_FLASH_CONTROL_PINS(p->clk, p->cs_n, p->mosi, p->miso);
    if ((r = slogic_write32(dev, SLOGIC_FLASH_DIVIDER, FLASH_DIVIDER)))
        return r;
    return slogic_write32(dev, SLOGIC_FLASH_CONTROL, dev->flash_control);
}

static int flash_cmd(struct slogic_dev *dev, uint8_t cmd)
{
    return flash_xfer(dev, &cmd, NULL, 1, 0);
}

static int flash_wait(struct slogic_dev *dev, double timeout_s)
{
    uint8_t tx[2] = { FLASH_CMD_RDSR, 0 }, rx[2];
    double t0 = slogic_time();
    int r;

    for (;;) {
        if ((r = flash_xfer(dev, tx, rx, 2, 0)))
            return r;
        if (!(rx[1] & 0x01))
            return SLOGIC_OK;
        if (slogic_time() - t0 > timeout_s)
            return SLOGIC_ERR_TIMEOUT;
    }
}

static int flash_check_region(uint32_t addr, size_t size)
{
    if ((uint64_t)addr + size > SLOGIC_FLASH_APP_BASE + SLOGIC_FLASH_APP_SIZE)
        return SLOGIC_ERR_ARG; /* Never above the application region (DFU loader). */
    return SLOGIC_OK;
}

/* Probe */
/*-------*/

int slogic_flash_jedec_id(struct slogic_dev *dev, uint8_t jedec[3])
{
    uint8_t tx[4] = { FLASH_CMD_RDID, 0, 0, 0 }, rx[4];
    int r = flash_xfer(dev, tx, rx, 4, 0);
    if (!r)
        memcpy(jedec, rx + 1, 3);
    return r;
}

static int flash_sfdp_ok(struct slogic_dev *dev)
{
    uint8_t tx[9] = { FLASH_CMD_RDSFDP, 0, 0, 0, 0, 0, 0, 0, 0 }, rx[9];
    if (flash_xfer(dev, tx, rx, 9, 0))
        return 0;
    return !memcmp(rx + 5, "SFDP", 4);
}

static int jedec_valid(const uint8_t id[3])
{
    return !(id[0] == 0x00 || id[0] == 0xff) && !(id[1] == 0xff && id[2] == 0xff);
}

int slogic_flash_probe(struct slogic_dev *dev, struct slogic_flash_pins *pins, uint8_t jedec[3])
{
    struct slogic_flash_pins p;
    uint8_t id[3], id2[3];
    int r;

    for (p.clk = 0; p.clk < SLOGIC_FLASH_NPADS; p.clk++)
    for (p.cs_n = 0; p.cs_n < SLOGIC_FLASH_NPADS; p.cs_n++)
    for (p.mosi = 0; p.mosi < SLOGIC_FLASH_NPADS; p.mosi++)
    for (p.miso = 0; p.miso < SLOGIC_FLASH_NPADS; p.miso++) {
        if (p.clk == p.cs_n || p.clk == p.mosi || p.clk == p.miso || p.cs_n == p.mosi ||
            p.cs_n == p.miso || p.mosi == p.miso)
            continue;
        if ((r = flash_set_pins(dev, &p)))
            return r;
        if ((r = slogic_flash_jedec_id(dev, id)))
            return r;
        if (!jedec_valid(id))
            continue;
        /* Confirmed: same ID again and SFDP signature (JEDEC JESD216 Flashes). */
        if ((r = slogic_flash_jedec_id(dev, id2)))
            return r;
        if (memcmp(id, id2, 3) || !flash_sfdp_ok(dev))
            continue;
        if (pins)
            *pins = p;
        if (jedec)
            memcpy(jedec, id, 3);
        dev->flash_open = 1;
        return SLOGIC_OK;
    }
    slogic_write32(dev, SLOGIC_FLASH_CONTROL, 0); /* Pads released. */
    return SLOGIC_ERR_FLASH;
}

int slogic_flash_open(struct slogic_dev *dev, const struct slogic_flash_pins *pins)
{
    uint8_t id[3];
    int r;

    if (!dev)
        return SLOGIC_ERR_ARG;
    if (!pins)
        return slogic_flash_probe(dev, NULL, NULL);
    if ((r = flash_set_pins(dev, pins)))
        return r;
    if ((r = slogic_flash_jedec_id(dev, id)))
        return r;
    if (!jedec_valid(id))
        return SLOGIC_ERR_FLASH;
    dev->flash_open = 1;
    return SLOGIC_OK;
}

int slogic_flash_close(struct slogic_dev *dev)
{
    if (!dev)
        return SLOGIC_ERR_ARG;
    dev->flash_open = 0;
    return slogic_write32(dev, SLOGIC_FLASH_CONTROL, 0); /* All pads inputs. */
}

/* Read/Erase/Write */
/*------------------*/

int slogic_flash_read(struct slogic_dev *dev, uint32_t addr, uint8_t *data, size_t size)
{
    uint8_t tx[SLOGIC_FLASH_BUFFER_SIZE], rx[SLOGIC_FLASH_BUFFER_SIZE];
    const size_t chunk = SLOGIC_FLASH_BUFFER_SIZE - 4;
    int r;

    if (!dev || !dev->flash_open)
        return SLOGIC_ERR_ARG;
    while (size) {
        size_t n = size > chunk ? chunk : size;
        memset(tx, 0, 4 + n);
        tx[0] = FLASH_CMD_READ;
        tx[1] = addr >> 16;
        tx[2] = addr >> 8;
        tx[3] = addr;
        if ((r = flash_xfer(dev, tx, rx, 4 + n, 0)))
            return r;
        memcpy(data, rx + 4, n);
        data += n;
        addr += n;
        size -= n;
    }
    return SLOGIC_OK;
}

int slogic_flash_erase(struct slogic_dev *dev, uint32_t addr, size_t size)
{
    uint32_t end;
    int r;

    if (!dev || !dev->flash_open || (addr % SLOGIC_FLASH_SECTOR))
        return SLOGIC_ERR_ARG;
    size = (size + SLOGIC_FLASH_SECTOR - 1)/SLOGIC_FLASH_SECTOR*SLOGIC_FLASH_SECTOR;
    if ((r = flash_check_region(addr, size)))
        return r;
    end = addr + size;
    while (addr < end) {
        int big = !(addr % 65536) && end - addr >= 65536;
        uint8_t tx[4] = { big ? FLASH_CMD_BE64 : FLASH_CMD_SE, addr >> 16, addr >> 8, addr };
        if ((r = flash_cmd(dev, FLASH_CMD_WREN)))
            return r;
        if ((r = flash_xfer(dev, tx, NULL, 4, 0)))
            return r;
        if ((r = flash_wait(dev, 3.0)))
            return r;
        addr += big ? 65536 : SLOGIC_FLASH_SECTOR;
    }
    return SLOGIC_OK;
}

int slogic_flash_write(struct slogic_dev *dev, uint32_t addr, const uint8_t *data, size_t size)
{
    uint8_t tx[4 + SLOGIC_FLASH_PAGE];
    int r;

    if (!dev || !dev->flash_open)
        return SLOGIC_ERR_ARG;
    if ((r = flash_check_region(addr, size)))
        return r;
    while (size) {
        size_t n = SLOGIC_FLASH_PAGE - (addr % SLOGIC_FLASH_PAGE); /* Within the page. */
        if (n > size)
            n = size;
        tx[0] = FLASH_CMD_PP;
        tx[1] = addr >> 16;
        tx[2] = addr >> 8;
        tx[3] = addr;
        memcpy(tx + 4, data, n);
        if ((r = flash_cmd(dev, FLASH_CMD_WREN)))
            return r;
        if ((r = flash_xfer(dev, tx, NULL, 4 + n, 0)))
            return r;
        if ((r = flash_wait(dev, 0.1)))
            return r;
        data += n;
        addr += n;
        size -= n;
    }
    return SLOGIC_OK;
}

/* Update */
/*--------*/

int slogic_flash_update(struct slogic_dev *dev, const uint8_t *image, size_t size,
                        slogic_progress_cb progress, void *user)
{
    const size_t step = 64*1024;
    uint8_t *check;
    int r;

    if (!dev || !image || !size)
        return SLOGIC_ERR_ARG;
    if ((r = flash_check_region(SLOGIC_FLASH_APP_BASE, size)))
        return r;
    if (!dev->flash_open && (r = slogic_flash_open(dev, NULL)))
        return r;
    check = malloc(step);
    if (!check)
        return SLOGIC_ERR_NOMEM;

    /* Erase, program, verify (64KB steps). */
    for (size_t o = 0; o < size; o += step) {
        size_t n = size - o > step ? step : size - o;
        if ((r = slogic_flash_erase(dev, SLOGIC_FLASH_APP_BASE + o, n)))
            goto out;
        if (progress)
            progress("erase", o + n, size, user);
    }
    for (size_t o = 0; o < size; o += step) {
        size_t n = size - o > step ? step : size - o;
        if ((r = slogic_flash_write(dev, SLOGIC_FLASH_APP_BASE + o, image + o, n)))
            goto out;
        if (progress)
            progress("program", o + n, size, user);
    }
    for (size_t o = 0; o < size; o += step) {
        size_t n = size - o > step ? step : size - o;
        if ((r = slogic_flash_read(dev, SLOGIC_FLASH_APP_BASE + o, check, n)))
            goto out;
        if (memcmp(check, image + o, n)) {
            r = SLOGIC_ERR_FLASH;
            goto out;
        }
        if (progress)
            progress("verify", o + n, size, user);
    }
    r = SLOGIC_OK;
out:
    free(check);
    return r;
}
