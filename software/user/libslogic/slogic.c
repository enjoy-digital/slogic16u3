/* SPDX-License-Identifier: BSD-2-Clause
 *
 * libslogic: device, registers, configuration and capture.
 *
 * This file is part of SLogic16U3.
 *
 * Copyright (c) 2026 Enjoy-Digital <enjoy-digital.fr>
 *
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "slogic.h"
#include "slogic_internal.h"
#include "slogic_regs.h"

/* Helpers */
/*---------*/

const char *slogic_strerror(int err)
{
    switch (err) {
    case SLOGIC_OK:           return "ok";
    case SLOGIC_ERR_ARG:      return "invalid argument";
    case SLOGIC_ERR_IO:       return "I/O error";
    case SLOGIC_ERR_TIMEOUT:  return "timeout";
    case SLOGIC_ERR_NODEV:    return "no device";
    case SLOGIC_ERR_DEVICE:   return "unexpected device";
    case SLOGIC_ERR_OVERFLOW: return "capture overflow (data lost)";
    case SLOGIC_ERR_END:      return "end of capture";
    case SLOGIC_ERR_NOMEM:    return "out of memory";
    case SLOGIC_ERR_FLASH:    return "flash error";
    default:                  return "error";
    }
}

double slogic_time(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec + ts.tv_nsec*1e-9;
}

void slogic_sleep_ms(unsigned ms)
{
    struct timespec ts = { ms/1000, (long)(ms % 1000)*1000000L };
    nanosleep(&ts, NULL);
}

/* Registers */
/*-----------*/

int slogic_read_burst(struct slogic_dev *dev, uint32_t addr, uint32_t *vals, size_t count)
{
    if (!dev)
        return SLOGIC_ERR_ARG;
    return dev->t->read_burst(dev->ctx, addr, vals, count);
}

int slogic_write_burst(struct slogic_dev *dev, uint32_t addr, const uint32_t *vals, size_t count)
{
    if (!dev)
        return SLOGIC_ERR_ARG;
    return dev->t->write_burst(dev->ctx, addr, vals, count);
}

int slogic_read32(struct slogic_dev *dev, uint32_t addr, uint32_t *val)
{
    return slogic_read_burst(dev, addr, val, 1);
}

int slogic_write32(struct slogic_dev *dev, uint32_t addr, uint32_t val)
{
    return slogic_write_burst(dev, addr, &val, 1);
}

int slogic_read64(struct slogic_dev *dev, uint32_t addr, uint64_t *val)
{
    uint32_t w[2];
    int r = slogic_read_burst(dev, addr, w, 2);
    if (r)
        return r;
    *val = ((uint64_t)w[0] << 32) | w[1];
    return SLOGIC_OK;
}

int slogic_write64(struct slogic_dev *dev, uint32_t addr, uint64_t val)
{
    uint32_t w[2] = { (uint32_t)(val >> 32), (uint32_t)val };
    return slogic_write_burst(dev, addr, w, 2);
}

/* Device */
/*--------*/

static int slogic_read_info(struct slogic_dev *dev)
{
    struct slogic_info *info = &dev->info;
    uint32_t regs[4];
    uint32_t ident[64];
    int r;

    memset(info, 0, sizeof(*info));
    if (dev->t->get_info)
        dev->t->get_info(dev->ctx, info);
    if ((r = slogic_read_burst(dev, SLOGIC_ANALYZER_MAGIC, regs, 4)))
        return r;
    info->magic      = regs[0];
    info->version    = regs[1];
    info->nchannels  = SLOGIC_CONFIG_NCHANNELS(regs[2]);
    info->ratio      = SLOGIC_CONFIG_RATIO(regs[2]);
    info->fifo_words = SLOGIC_CONFIG_FIFO_DEPTH(regs[2]);
    info->base_rate  = regs[3];
    if (info->magic != SLOGIC_MAGIC || (info->version >> 16) != 1 || !info->ratio)
        return SLOGIC_ERR_DEVICE;

    /* Identifier: one character per CSR word, NUL terminated. */
    if ((r = slogic_read_burst(dev, SLOGIC_IDENTIFIER_BASE, ident, 64)))
        return r;
    for (size_t i = 0; i < 64 && i < sizeof(info->ident) - 1; i++) {
        info->ident[i] = (char)(ident[i] & 0xff);
        if (!info->ident[i])
            break;
    }
    return SLOGIC_OK;
}

int slogic_open(const char *uri, struct slogic_dev **devp)
{
    struct slogic_dev *dev;
    const char *args;
    int r;

    if (!devp)
        return SLOGIC_ERR_ARG;
    *devp = NULL;
    if (!uri || !uri[0])
        uri = "usb";
    args = strchr(uri, ':');
    args = args ? args + 1 : "";

    dev = calloc(1, sizeof(*dev));
    if (!dev)
        return SLOGIC_ERR_NOMEM;
    if (!strncmp(uri, "usb", 3))
        r = slogic_usb_open(args, dev);
    else if (!strncmp(uri, "sim", 3))
        r = slogic_sim_open(args, dev);
    else
        r = SLOGIC_ERR_ARG;
    if (r) {
        free(dev);
        return r;
    }
    if ((r = slogic_read_info(dev))) {
        slogic_close(dev);
        return r;
    }
    *devp = dev;
    return SLOGIC_OK;
}

void slogic_close(struct slogic_dev *dev)
{
    if (!dev)
        return;
    if (dev->capturing)
        slogic_capture_stop(dev, NULL);
    if (dev->t && dev->t->close)
        dev->t->close(dev->ctx);
    free(dev);
}

int slogic_get_info(struct slogic_dev *dev, struct slogic_info *info)
{
    if (!dev || !info)
        return SLOGIC_ERR_ARG;
    *info = dev->info;
    return SLOGIC_OK;
}

int slogic_measure_sample_clock(struct slogic_dev *dev, unsigned duration_ms, double *freq)
{
    uint32_t c0, c1, wraps = 0, last;
    double t0, t1;
    int r;

    /* Counter read every 10ms at most (32-bit counter: 42s at 100MHz, wraps counted). */
    if ((r = slogic_read32(dev, SLOGIC_ANALYZER_CLK_COUNT, &c0)))
        return r;
    t0   = slogic_time();
    last = c0;
    do {
        slogic_sleep_ms(duration_ms < 10 ? duration_ms : 10);
        if ((r = slogic_read32(dev, SLOGIC_ANALYZER_CLK_COUNT, &c1)))
            return r;
        if (c1 < last)
            wraps++;
        last = c1;
        t1 = slogic_time();
    } while ((t1 - t0)*1e3 < duration_ms);
    *freq = (((uint64_t)wraps << 32) + c1 - c0)/(t1 - t0);
    return SLOGIC_OK;
}

/* Configuration */
/*---------------*/

static int slogic_mode(unsigned channels)
{
    switch (channels) {
    case 4:  return SLOGIC_MODE_4CH;
    case 8:  return SLOGIC_MODE_8CH;
    case 16: return SLOGIC_MODE_16CH;
    default: return -1;
    }
}

/* Gateware datapath: up to 32 bits per sample clock cycle (ceil(ratio/divider) samples). */
static int slogic_divider_ok(const struct slogic_info *info, unsigned channels, uint64_t divider)
{
    uint64_t kmax = (info->ratio + divider - 1)/divider;
    return divider >= 1 && divider <= 0xffffffffULL && kmax*channels <= 32;
}

int slogic_config_check(const struct slogic_info *info, const struct slogic_config *config,
                        char *msg, size_t msg_size)
{
    char dummy[1];
    if (!msg) {
        msg      = dummy;
        msg_size = sizeof(dummy);
    }
    msg[0] = '\0';
    if (!info || !config)
        return SLOGIC_ERR_ARG;
    if (slogic_mode(config->channels) < 0) {
        snprintf(msg, msg_size, "%u channels not supported (4, 8 or 16)", config->channels);
        return SLOGIC_ERR_ARG;
    }
    if (!config->samplerate || info->base_rate % config->samplerate) {
        snprintf(msg, msg_size, "samplerate %llu Hz is not %llu Hz/N",
                 (unsigned long long)config->samplerate, (unsigned long long)info->base_rate);
        return SLOGIC_ERR_ARG;
    }
    if (!slogic_divider_ok(info, config->channels, info->base_rate/config->samplerate)) {
        snprintf(msg, msg_size, "samplerate %llu Hz too high for %u channels (max %llu Hz)",
                 (unsigned long long)config->samplerate, config->channels,
                 (unsigned long long)slogic_max_samplerate(info, config->channels));
        return SLOGIC_ERR_ARG;
    }
    if (config->pattern > SLOGIC_PATTERN_COUNTER) {
        snprintf(msg, msg_size, "unknown pattern %u", config->pattern);
        return SLOGIC_ERR_ARG;
    }
    return SLOGIC_OK;
}

uint64_t slogic_max_samplerate(const struct slogic_info *info, unsigned channels)
{
    for (uint64_t d = 1; d <= info->ratio*4ULL; d++)
        if (slogic_divider_ok(info, channels, d) && info->base_rate % d == 0)
            return info->base_rate/d;
    return 0;
}

int slogic_samplerates(const struct slogic_info *info, unsigned channels, uint64_t *rates, int max)
{
    /* Standard steps (1/2/2.5/4/5/8 x 10^n, from 1kHz), plus base_rate/d for small dividers. */
    static const unsigned steps[] = { 10, 20, 25, 40, 50, 80 };
    uint64_t max_rate = slogic_max_samplerate(info, channels);
    int n = 0;

    for (uint64_t decade = 100; decade <= info->base_rate && n < max; decade *= 10) {
        for (size_t i = 0; i < sizeof(steps)/sizeof(steps[0]) && n < max; i++) {
            uint64_t rate = decade*steps[i];
            if (rate > max_rate || info->base_rate % rate)
                continue;
            if (n && rates[n - 1] >= rate)
                continue;
            rates[n++] = rate;
        }
    }
    /* Highest rates (base/4, base/2, base: not always a standard step, ex: 160MHz = 800MHz/5). */
    for (uint64_t d = 8; d >= 1 && n < max; d--) {
        uint64_t rate = info->base_rate/d;
        if (info->base_rate % d || rate > max_rate || (n && rates[n - 1] >= rate))
            continue;
        rates[n++] = rate;
    }
    return n;
}

/* Capture */
/*---------*/

int slogic_capture_start(struct slogic_dev *dev, const struct slogic_config *config)
{
    int r;

    if (!dev || !config)
        return SLOGIC_ERR_ARG;
    if ((r = slogic_config_check(&dev->info, config, NULL, 0)))
        return r;
    if (dev->capturing)
        slogic_capture_stop(dev, NULL);

    dev->config      = *config;
    dev->bytes       = 0;
    dev->ended       = 0;
    dev->bytes_limit = config->limit ? 8*((slogic_packed_bytes(config->channels, config->limit) + 7)/8) : 0;

    /* Stopped, configured, stream started (stale data flushed), then enabled. */
    uint32_t control = SLOGIC_CONTROL_MODE(slogic_mode(config->channels)) |
                       SLOGIC_CONTROL_PATTERN(config->pattern);
    if ((r = slogic_write32(dev, SLOGIC_ANALYZER_CONTROL, control)))
        return r;
    if ((r = slogic_write32(dev, SLOGIC_ANALYZER_DIVIDER, (uint32_t)(dev->info.base_rate/config->samplerate))))
        return r;
    if ((r = slogic_write64(dev, SLOGIC_ANALYZER_LIMIT, config->limit)))
        return r;
    if ((r = dev->t->stream_start(dev->ctx)))
        return r;
    if ((r = slogic_write32(dev, SLOGIC_ANALYZER_CONTROL, control | SLOGIC_CONTROL_ENABLE)))
        return r;
    dev->capturing = 1;
    return SLOGIC_OK;
}

int slogic_capture_read(struct slogic_dev *dev, const uint8_t **data, size_t *size, int timeout_ms)
{
    int last = 0;
    int r;

    if (!dev || !data || !size)
        return SLOGIC_ERR_ARG;
    if (!dev->capturing || dev->ended)
        return SLOGIC_ERR_END;
    r = dev->t->stream_read(dev->ctx, data, size, &last, timeout_ms);
    if (r)
        return r;
    dev->bytes += *size;
    /* With a limit, the data beyond the limit (padding of the last word) is not returned. */
    if (dev->bytes_limit && dev->bytes >= dev->bytes_limit) {
        uint64_t keep = slogic_packed_bytes(dev->config.channels, dev->config.limit);
        uint64_t prev = dev->bytes - *size;
        *size = keep > prev ? (size_t)(keep - prev) : 0;
        dev->ended = 1;
    }
    if (last && !dev->ended) {
        /* Stream ended before the limit (stopped/overflow): status tells why. */
        struct slogic_status st;
        dev->ended = 1;
        if (!slogic_capture_status(dev, &st) && st.overflow)
            return *size ? SLOGIC_OK : SLOGIC_ERR_OVERFLOW;
    }
    return SLOGIC_OK;
}

int slogic_capture_status(struct slogic_dev *dev, struct slogic_status *st)
{
    uint32_t status;
    int r;

    if (!dev || !st)
        return SLOGIC_ERR_ARG;
    memset(st, 0, sizeof(*st));
    if ((r = slogic_read32(dev, SLOGIC_ANALYZER_STATUS, &status)))
        return r;
    if ((r = slogic_read64(dev, SLOGIC_ANALYZER_COUNT, &st->samples)))
        return r;
    st->running  = !!(status & SLOGIC_STATUS_RUNNING);
    st->done     = !!(status & SLOGIC_STATUS_DONE);
    st->overflow = !!(status & SLOGIC_STATUS_OVERFLOW);
    st->error    = !!(status & SLOGIC_STATUS_ERROR);
    st->bytes    = dev->bytes;
    return SLOGIC_OK;
}

int slogic_capture_stop(struct slogic_dev *dev, struct slogic_status *st)
{
    uint32_t control;
    int r;

    if (!dev)
        return SLOGIC_ERR_ARG;
    if ((r = slogic_read32(dev, SLOGIC_ANALYZER_CONTROL, &control)))
        return r;
    if ((r = slogic_write32(dev, SLOGIC_ANALYZER_CONTROL, control & ~SLOGIC_CONTROL_ENABLE)))
        return r;
    dev->t->stream_stop(dev->ctx);
    dev->capturing = 0;
    if (st)
        return slogic_capture_status(dev, st);
    return SLOGIC_OK;
}

/* Data */
/*------*/

unsigned slogic_unitsize(unsigned channels)
{
    return channels > 8 ? 2 : 1;
}

uint64_t slogic_packed_bytes(unsigned channels, uint64_t nsamples)
{
    return (nsamples*channels + 7)/8;
}

void slogic_unpack(unsigned channels, const uint8_t *data, uint64_t first, uint64_t nsamples, uint8_t *out)
{
    switch (channels) {
    case 4:
        for (uint64_t i = 0; i < nsamples; i++) {
            uint64_t s = first + i;
            out[i] = (data[(s - (first & ~1ULL))/2] >> (4*(s & 1))) & 0xf;
        }
        break;
    case 8:
        memcpy(out, data, nsamples);
        break;
    default:
        memcpy(out, data, 2*nsamples);
        break;
    }
}
