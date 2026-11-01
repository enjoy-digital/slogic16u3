/* SPDX-License-Identifier: BSD-2-Clause
 *
 * libslogic16u3: device, registers, configuration and capture.
 *
 * This file is part of SLogic16U3.
 *
 * Copyright (c) 2026 Enjoy-Digital <enjoy-digital.fr>
 *
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
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
    if (info->magic != SLOGIC_MAGIC || (info->version >> 16) != 0 || !info->ratio)
        return SLOGIC_ERR_DEVICE;
    if ((info->version & 0xffff) >= 3) {
        uint32_t oc;
        if ((r = slogic_read32(dev, SLOGIC_ANALYZER_OUT_CONFIG, &oc)))
            return r;
        info->naux            = SLOGIC_OUT_CONFIG_NAUX(oc);
        info->output_channels = (uint16_t)SLOGIC_OUT_CONFIG_CHANNELS(oc);
    }

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
    if (config->pattern > SLOGIC_PATTERN_GENERATOR) {
        snprintf(msg, msg_size, "unknown pattern %u", config->pattern);
        return SLOGIC_ERR_ARG;
    }
    if (config->trigger_enable) {
        const struct slogic_trigger *t = &config->trigger;
        uint32_t used = t->zero | t->one | t->rising | t->falling;
        if (!slogic_has_trigger(info)) {
            snprintf(msg, msg_size, "hardware trigger not supported by this gateware (0.2 or later)");
            return SLOGIC_ERR_ARG;
        }
        if (config->channels < 16 && (used >> config->channels)) {
            snprintf(msg, msg_size, "trigger on a channel not captured (%u channels)", config->channels);
            return SLOGIC_ERR_ARG;
        }
        if (t->zero & t->one) {
            snprintf(msg, msg_size, "trigger: channel both low and high");
            return SLOGIC_ERR_ARG;
        }
        if (config->pretrigger > slogic_max_pretrigger(info, config->channels)) {
            snprintf(msg, msg_size, "pre-trigger %llu samples, max %llu with %u channels",
                     (unsigned long long)config->pretrigger,
                     (unsigned long long)slogic_max_pretrigger(info, config->channels), config->channels);
            return SLOGIC_ERR_ARG;
        }
    }
    return SLOGIC_OK;
}

/* Samples per 64-bit stream word. */
static unsigned slogic_spw(unsigned channels)
{
    return 64/channels;
}

int slogic_has_trigger(const struct slogic_info *info)
{
    return info && (info->version & 0xffff) >= 2;
}

uint64_t slogic_max_pretrigger(const struct slogic_info *info, unsigned channels)
{
    if (!slogic_has_trigger(info) || slogic_mode(channels) < 0 || info->fifo_words <= 2*SLOGIC_PRETRIGGER_MARGIN)
        return 0;
    return (uint64_t)(info->fifo_words - 2*SLOGIC_PRETRIGGER_MARGIN)*slogic_spw(channels);
}

int slogic_trigger_parse(const char *s, struct slogic_trigger *t)
{
    if (!s || !t)
        return SLOGIC_ERR_ARG;
    memset(t, 0, sizeof(*t));
    while (*s) {
        unsigned ch;
        int n = 0;
        char c;
        if (sscanf(s, "%*[Cc]%*[Hh]%u=%c%n", &ch, &c, &n) != 2 || !n || ch > 15)
            return SLOGIC_ERR_ARG;
        uint16_t bit = (uint16_t)(1u << ch);
        switch (c) {
        case '0': t->zero    |= bit; break;
        case '1': t->one     |= bit; break;
        case 'r': t->rising  |= bit; break;
        case 'f': t->falling |= bit; break;
        case 'e': t->rising  |= bit; t->falling |= bit; break;
        default:  return SLOGIC_ERR_ARG;
        }
        s += n;
        if (*s == ',')
            s++;
        else if (*s)
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

    dev->config        = *config;
    dev->bytes         = 0;
    dev->ended         = 0;
    dev->trig_known    = 0;
    dev->trig_sample   = 0;
    dev->trig_offset   = 0;
    dev->samples_limit = config->trigger_enable ? 0 : config->limit;
    dev->bytes_limit   = dev->samples_limit ? 8*((slogic_packed_bytes(config->channels, dev->samples_limit) + 7)/8) : 0;

    /* Stopped, configured, stream started (stale data flushed), then enabled. */
    uint32_t control = SLOGIC_CONTROL_MODE(slogic_mode(config->channels)) |
                       SLOGIC_CONTROL_PATTERN(config->pattern);
    uint64_t limit   = config->limit;
    uint32_t pre     = 0;
    if (config->trigger_enable) {
        /* Gateware limit counted from the trigger cycle (up to ratio - 1 samples before the trigger,
         * >= 16), trimmed here; pre-trigger in stream words. */
        unsigned spw = slogic_spw(config->channels);
        if (limit)
            limit = limit + dev->info.ratio < 16 ? 16 : limit + dev->info.ratio;
        pre = (uint32_t)((config->pretrigger + spw - 1)/spw);
    }
    if ((r = slogic_write32(dev, SLOGIC_ANALYZER_CONTROL, control)))
        return r;
    if ((r = slogic_write32(dev, SLOGIC_ANALYZER_DIVIDER, (uint32_t)(dev->info.base_rate/config->samplerate))))
        return r;
    if ((r = slogic_write64(dev, SLOGIC_ANALYZER_LIMIT, limit)))
        return r;
    if (slogic_has_trigger(&dev->info)) {
        const struct slogic_trigger *t = &config->trigger;
        if ((r = slogic_write32(dev, SLOGIC_ANALYZER_TRIGGER, config->trigger_enable ? SLOGIC_TRIGGER_ENABLE : 0)))
            return r;
        if ((r = slogic_write32(dev, SLOGIC_ANALYZER_TRIGGER_LEVEL, (uint32_t)t->one << 16 | t->zero)))
            return r;
        if ((r = slogic_write32(dev, SLOGIC_ANALYZER_TRIGGER_EDGE, (uint32_t)t->falling << 16 | t->rising)))
            return r;
        if ((r = slogic_write32(dev, SLOGIC_ANALYZER_PRETRIGGER, pre)))
            return r;
    }
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
    if (dev->config.trigger_enable && !dev->trig_known && (*size || last)) {
        /* First data: forwarded once triggered, trigger position stable. */
        struct slogic_status st;
        if ((r = slogic_capture_status(dev, &st)))
            return r;
        if (st.triggered && dev->trig_known && dev->config.limit) {
            dev->samples_limit = dev->trig_offset + dev->config.limit;
            dev->bytes_limit   = 8*((slogic_packed_bytes(dev->config.channels, dev->samples_limit) + 7)/8);
        }
    }
    dev->bytes += *size;
    /* With a limit, the data beyond the limit (padding of the last word) is not returned. */
    if (dev->bytes_limit && dev->bytes >= dev->bytes_limit) {
        uint64_t keep = slogic_packed_bytes(dev->config.channels, dev->samples_limit);
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
    if (dev->config.trigger_enable && (status & SLOGIC_STATUS_TRIGGERED)) {
        if (!dev->trig_known) {
            /* Stream from word max(0, W - pretrigger) (gateware/analyzer.py). */
            unsigned spw = slogic_spw(dev->config.channels);
            uint64_t pre = (dev->config.pretrigger + spw - 1)/spw;
            uint64_t tc, w;
            if ((r = slogic_read64(dev, SLOGIC_ANALYZER_TRIGGER_COUNT, &tc)))
                return r;
            w = tc/spw;
            dev->trig_sample = tc;
            dev->trig_offset = tc - (w > pre ? w - pre : 0)*spw;
            dev->trig_known  = 1;
        }
        st->triggered      = 1;
        st->trigger_sample = dev->trig_sample;
        st->trigger_offset = dev->trig_offset;
    }
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

/* Outputs */
/*---------*/

static unsigned popcount16(uint16_t v)
{
    unsigned n = 0;
    for (; v; v &= v - 1)
        n++;
    return n;
}

unsigned slogic_output_count(const struct slogic_info *info)
{
    if (!info || (info->version & 0xffff) < 3)
        return 0;
    return info->naux + popcount16(info->output_channels);
}

int slogic_output_channel(const struct slogic_info *info, unsigned out)
{
    if (!info || out >= slogic_output_count(info) || out < info->naux)
        return -1;
    unsigned k = out - info->naux;
    for (int c = 0; c < 16; c++)
        if ((info->output_channels >> c) & 1) {
            if (!k)
                return c;
            k--;
        }
    return -1;
}

int slogic_output_name(const struct slogic_info *info, unsigned out, char *name, size_t size)
{
    if (!info || !name || out >= slogic_output_count(info))
        return SLOGIC_ERR_ARG;
    if (out < info->naux)
        snprintf(name, size, "AUX%u", out);
    else
        snprintf(name, size, "CH%d", slogic_output_channel(info, out));
    return SLOGIC_OK;
}

int slogic_output_find(const struct slogic_info *info, const char *name)
{
    char n[16];
    for (unsigned i = 0; name && i < slogic_output_count(info); i++)
        if (!slogic_output_name(info, i, n, sizeof(n)) && !strcasecmp(n, name))
            return (int)i;
    return -1;
}

int slogic_output_source_parse(const char *s, unsigned *source)
{
    static const char *names[] = { "gpio", "trigger", "armed", "capture", "periodic" };
    if (!s || !source)
        return SLOGIC_ERR_ARG;
    for (unsigned i = 0; i < sizeof(names)/sizeof(names[0]); i++)
        if (!strcasecmp(s, names[i])) {
            *source = i;
            return SLOGIC_OK;
        }
    if (!strncasecmp(s, "gen", 3) && s[3] >= '0' && s[3] <= '7' && !s[4]) {
        *source = SLOGIC_OUT_GENERATOR + (unsigned)(s[3] - '0');
        return SLOGIC_OK;
    }
    return SLOGIC_ERR_ARG;
}

const char *slogic_output_source_name(unsigned source, char *buf, size_t size)
{
    static const char *names[] = { "gpio", "trigger", "armed", "capture", "periodic" };
    if (source < sizeof(names)/sizeof(names[0]))
        snprintf(buf, size, "%s", names[source]);
    else if (source >= SLOGIC_OUT_GENERATOR && source < SLOGIC_OUT_GENERATOR + 8)
        snprintf(buf, size, "gen%u", source - SLOGIC_OUT_GENERATOR);
    else
        snprintf(buf, size, "?");
    return buf;
}

int slogic_output_get(struct slogic_dev *dev, unsigned out, struct slogic_output *o)
{
    uint32_t ctrl, sel;
    int r;
    if (!dev || !o || out >= slogic_output_count(&dev->info))
        return SLOGIC_ERR_ARG;
    if ((r = slogic_read32(dev, SLOGIC_ANALYZER_OUT_CTRL, &ctrl)) || (r = slogic_read32(dev, SLOGIC_ANALYZER_OUT_SEL, &sel)))
        return r;
    o->enable = (ctrl >> out) & 1;
    o->invert = (ctrl >> (8 + out)) & 1;
    o->level  = (ctrl >> (16 + out)) & 1;
    o->source = (sel >> (4*out)) & 0xf;
    return SLOGIC_OK;
}

int slogic_output_set(struct slogic_dev *dev, unsigned out, const struct slogic_output *o)
{
    uint32_t ctrl, sel;
    int r;
    if (!dev || !o || out >= slogic_output_count(&dev->info) || o->source > 0xf)
        return SLOGIC_ERR_ARG;
    if ((r = slogic_read32(dev, SLOGIC_ANALYZER_OUT_CTRL, &ctrl)) || (r = slogic_read32(dev, SLOGIC_ANALYZER_OUT_SEL, &sel)))
        return r;
    /* Source/levels first, then the output enable (no glitch from a previous source). */
    sel  = (sel & ~(0xfu << (4*out))) | (o->source << (4*out));
    ctrl = (ctrl & ~((1u << (8 + out)) | (1u << (16 + out)))) |
           ((uint32_t)!!o->invert << (8 + out)) | ((uint32_t)!!o->level << (16 + out));
    if ((r = slogic_write32(dev, SLOGIC_ANALYZER_OUT_SEL, sel)) ||
        (r = slogic_write32(dev, SLOGIC_ANALYZER_OUT_CTRL, ctrl & ~(1u << out))))
        return r;
    if (o->enable)
        return slogic_write32(dev, SLOGIC_ANALYZER_OUT_CTRL, ctrl | (1u << out));
    return SLOGIC_OK;
}

int slogic_output_timing(struct slogic_dev *dev, uint32_t width, uint32_t delay, uint32_t period, uint32_t high)
{
    int r;
    if (!dev || slogic_output_count(&dev->info) == 0 || width > 0xff || delay > 0xffffff ||
        !period || period > 0x1000000 || high > period)
        return SLOGIC_ERR_ARG;
    if ((r = slogic_write32(dev, SLOGIC_ANALYZER_OUT_PULSE, delay | width << 24)) ||
        (r = slogic_write32(dev, SLOGIC_ANALYZER_OUT_HIGH,  high)) ||
        (r = slogic_write32(dev, SLOGIC_ANALYZER_OUT_LOW,   period - high)))
        return r;
    return SLOGIC_OK;
}

int slogic_output_levels(struct slogic_dev *dev, uint32_t *levels)
{
    if (!dev || !levels || slogic_output_count(&dev->info) == 0)
        return SLOGIC_ERR_ARG;
    return slogic_read32(dev, SLOGIC_ANALYZER_OUT_LEVELS, levels);
}

int slogic_outputs_off(struct slogic_dev *dev)
{
    uint32_t ctrl;
    int r;
    if (!dev || slogic_output_count(&dev->info) == 0)
        return SLOGIC_ERR_ARG;
    if ((r = slogic_read32(dev, SLOGIC_ANALYZER_OUT_CTRL, &ctrl)))
        return r;
    return slogic_write32(dev, SLOGIC_ANALYZER_OUT_CTRL, ctrl & ~0xffu);
}

/* Data */
/*------*/

uint16_t slogic_generator_sample(uint64_t t)
{
    static const uint8_t spi[4] = { 0x53, 0x4c, 0x4f, 0x47 }; /* "SLOG". */
    const char    *text = SLOGIC_GENERATOR_TEXT;
    const uint64_t len  = sizeof(SLOGIC_GENERATOR_TEXT) - 1;
    uint64_t c = t/8;                     /* Cycle (UART: not wrapped). */
    uint32_t n = (uint32_t)(c & 0x3ffff); /* 18-bit cycle counter. */
    unsigned j = t & 7;

    unsigned bit  = (c/100) % 12;
    uint8_t  ch   = (uint8_t)text[(c/1200) % len];
    unsigned uart = bit == 2 ? 0 : (bit >= 3 && bit <= 10) ? (ch >> (bit - 3)) & 1 : 1; /* Idle x2, start, data, stop. */
    unsigned cs_n = (n % 512) >= 256;
    unsigned sck  = cs_n ? 0 : (n >> 2) & 1;
    unsigned mosi = cs_n ? 0 : (spi[(n >> 6) & 3] >> (7 - ((n >> 3) & 7))) & 1;
    unsigned pwm  = (n % 1024) < 4*((n >> 10) % 256);
    unsigned clk  = j >= 4;
    unsigned gray = (n & 0x3ff) ^ ((n & 0x3ff) >> 1);
    return (uint16_t)(uart | cs_n << 1 | sck << 2 | mosi << 3 | pwm << 4 | clk << 5 | gray << 6);
}

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
