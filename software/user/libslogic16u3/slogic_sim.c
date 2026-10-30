/* SPDX-License-Identifier: BSD-2-Clause
 *
 * libslogic16u3: simulated device (software model of the gateware), for tests and development without
 * hardware: "sim[:rate=<bytes/s>][,pins=<clk><cs_n><mosi><miso>][,fast]".
 *
 * - Registers: Analyzer (info, control, status, counters), Flash, identifier, USB3 PHY status.
 * - Capture stream: counter test pattern (sample i = i*divider, as the gateware), pattern generator
 *   (slogic_generator_sample(), as the gateware) or synthetic inputs (CH0:
 *   UART 1Mbaud "Hello from SLogic16U3!", CHn: square wave toggling every 2^(n+2) base samples), packed as
 *   the gateware (32-bit packer, last word
 *   flushed), paced at the samplerate (fast: as fast as possible). rate: emulated host bandwidth
 *   (overflow when the capture needs more, after the FIFO).
 * - Flash: 16MB SPI Flash (W25Q128-like: JEDEC ID ef 40 18) on hidden pads (pins), golden image area
 *   (0x800000-) filled with 0xa5.
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

/* Constants */
/*-----------*/

#define SIM_BASE_RATE   800000000ULL
#define SIM_SAMPLE_CLK  100000000ULL
#define SIM_RATIO       8
#define SIM_FIFO_WORDS  2048
#define SIM_BUF_SIZE    (256*1024)
#define SIM_FLASH_SIZE  (16*1024*1024)
#define SIM_IDENT       "SLogic16U3 LiteX (simulated device)"

/* Context */
/*---------*/

struct sim_flash {
    uint8_t *mem;
    int      cs;          /* Selected. */
    uint8_t  cmd[4 + SLOGIC_FLASH_BUFFER_SIZE];
    size_t   n;           /* Bytes received since CS asserted. */
    int      wel;
};

struct sim {
    /* Analyzer. */
    uint32_t control, divider, status;
    uint64_t limit, count;
    double   t0;
    /* Stream. */
    int      streaming, ended, overflow_after_words;
    uint64_t sample;      /* Next captured sample index. */
    uint64_t bits;        /* Bits sent. */
    uint8_t  buf[SIM_BUF_SIZE + 16];
    uint64_t pack;        /* Packer. */
    unsigned pack_bits;
    double   rate;        /* Emulated host bandwidth (bytes/s, 0: unlimited). */
    double   t_start;     /* Capture start (data paced at the samplerate). */
    int      realtime;    /* Data paced at the samplerate (default, "fast" disables it). */
    uint64_t words_sent;  /* 32-bit words. */
    /* Flash. */
    uint32_t flash_control, flash_divider, flash_xfer;
    uint8_t  tx[SLOGIC_FLASH_BUFFER_SIZE], rx[SLOGIC_FLASH_BUFFER_SIZE];
    unsigned pins[4];
    struct sim_flash flash;
    int      flash_writes_golden; /* Golden area writes (must stay 0). */
};

/* Flash Model */
/*-------------*/

static uint8_t sim_flash_byte(struct sim *s, uint8_t b)
{
    struct sim_flash *f = &s->flash;
    uint8_t out = 0xff;
    size_t n = f->n;

    if (n < sizeof(f->cmd))
        f->cmd[n] = b;
    f->n++;
    if (n == 0)
        return 0xff;
    switch (f->cmd[0]) {
    case 0x9f: { static const uint8_t id[3] = { 0xef, 0x40, 0x18 }; out = n <= 3 ? id[n - 1] : 0x00; break; }
    case 0x05: out = (uint8_t)(f->wel << 1); break;
    case 0x5a: { static const char sig[4] = { 'S', 'F', 'D', 'P' }; out = (n >= 5 && n < 9) ? (uint8_t)sig[n - 5] : 0x00; break; }
    case 0x03:
        if (n >= 4) {
            uint32_t addr = (f->cmd[1] << 16) | (f->cmd[2] << 8) | f->cmd[3];
            out = f->mem[(addr + n - 4) % SIM_FLASH_SIZE];
        }
        break;
    }
    return out;
}

static void sim_flash_deselect(struct sim *s)
{
    struct sim_flash *f = &s->flash;
    size_t n = f->n < sizeof(f->cmd) ? f->n : sizeof(f->cmd);

    if (!f->cs)
        return;
    f->cs = 0;
    if (!n)
        return;
    switch (f->cmd[0]) {
    case 0x06: f->wel = 1; break;
    case 0x04: f->wel = 0; break;
    case 0x02:
        if (f->wel && n > 4) {
            uint32_t addr = (f->cmd[1] << 16) | (f->cmd[2] << 8) | f->cmd[3];
            for (size_t i = 4; i < n; i++) {
                uint32_t a = (addr & ~0xffu) | ((addr + i - 4) & 0xff);
                if (a >= SLOGIC_FLASH_APP_SIZE)
                    s->flash_writes_golden++;
                f->mem[a % SIM_FLASH_SIZE] &= f->cmd[i];
            }
        }
        f->wel = 0;
        break;
    case 0x20: case 0x52: case 0xd8:
        if (f->wel && n == 4) {
            uint32_t size = f->cmd[0] == 0x20 ? 4096 : f->cmd[0] == 0x52 ? 32768 : 65536;
            uint32_t addr = ((f->cmd[1] << 16) | (f->cmd[2] << 8) | f->cmd[3]) & ~(size - 1);
            if (addr >= SLOGIC_FLASH_APP_SIZE)
                s->flash_writes_golden++;
            memset(&f->mem[addr % SIM_FLASH_SIZE], 0xff, size);
        }
        f->wel = 0;
        break;
    }
}

static void sim_flash_xfer(struct sim *s)
{
    unsigned length = s->flash_xfer & 0xffff;
    int hold = !!(s->flash_xfer & SLOGIC_FLASH_XFER_HOLD);
    unsigned clk  = (s->flash_control >> 4) & 7;
    unsigned cs_n = (s->flash_control >> 8) & 7;
    unsigned mosi = (s->flash_control >> 12) & 7;
    unsigned miso = (s->flash_control >> 16) & 7;
    int connected = (s->flash_control & SLOGIC_FLASH_CONTROL_ENABLE) && clk == s->pins[0] &&
                    cs_n == s->pins[1] && mosi == s->pins[2] && miso == s->pins[3];

    if (length > SLOGIC_FLASH_BUFFER_SIZE)
        length = SLOGIC_FLASH_BUFFER_SIZE;
    if (connected && !s->flash.cs) {
        s->flash.cs = 1;
        s->flash.n  = 0;
    }
    for (unsigned i = 0; i < length; i++)
        s->rx[i] = connected ? sim_flash_byte(s, s->tx[i]) : 0xff;
    if (!hold)
        sim_flash_deselect(s);
}

/* Capture Model */
/*---------------*/

static unsigned sim_channels(const struct sim *s)
{
    switch ((s->control >> 4) & 3) {
    case SLOGIC_MODE_4CH: return 4;
    case SLOGIC_MODE_8CH: return 8;
    default:              return 16;
    }
}

/* Simulated UART on CH0: SIM_UART_TEXT repeated, 1Mbaud 8N1 (800 base samples per bit), idle gaps. */
#define SIM_UART_TEXT  "Hello from SLogic16U3!\r\n"
#define SIM_UART_BIT   800
#define SIM_UART_FRAME 12 /* Start + 8 data + stop + 2 idle bits. */

static int sim_uart(uint64_t t)
{
    const size_t len = sizeof(SIM_UART_TEXT) - 1;
    uint64_t bit   = t/SIM_UART_BIT;
    uint64_t frame = (bit/SIM_UART_FRAME) % (len + 4);
    unsigned pos   = bit % SIM_UART_FRAME;

    if (frame < 4)
        return 1; /* Idle before each message. */
    uint8_t c = (uint8_t)SIM_UART_TEXT[frame - 4];
    if (pos == 0)
        return 0; /* Start bit. */
    if (pos <= 8)
        return (c >> (pos - 1)) & 1;
    return 1;     /* Stop/idle. */
}

static uint16_t sim_sample(const struct sim *s, uint64_t i)
{
    uint64_t t = i*(s->divider ? s->divider : 1); /* Base sample index. */
    if (((s->control >> 8) & 3) == SLOGIC_PATTERN_COUNTER)
        return (uint16_t)t;
    if (((s->control >> 8) & 3) == SLOGIC_PATTERN_GENERATOR)
        return slogic_generator_sample(t);
    /* CH0: UART, CHn (n >= 1): square wave toggling every 2^(n+2) base samples. */
    return (uint16_t)(((t >> 2) & ~1u) | sim_uart(t));
}

static void sim_start(struct sim *s)
{
    unsigned ch = sim_channels(s);
    double need = (double)SIM_BASE_RATE/(s->divider ? s->divider : 1)*ch/8;

    s->sample     = 0;
    s->count      = 0;
    s->pack       = 0;
    s->pack_bits  = 0;
    s->words_sent = 0;
    s->ended      = 0;
    s->status     = SLOGIC_STATUS_RUNNING;
    s->t_start    = slogic_time();
    /* Invalid configuration: more than 32 bits per sample clock cycle. */
    unsigned d = s->divider ? s->divider : 1;
    if (((SIM_RATIO + d - 1)/d)*ch > 32) {
        s->status = SLOGIC_STATUS_DONE | SLOGIC_STATUS_ERROR;
        s->overflow_after_words = 1;
        return;
    }
    /* Host too slow: FIFO filled then overflow. */
    s->overflow_after_words = (s->rate > 0 && need > s->rate) ? 2*SIM_FIFO_WORDS + 4096 : -1;
}

/* Generate up to max bytes (multiple of 8), returns the size, *last at the end of the capture. */
static size_t sim_generate(struct sim *s, uint8_t *out, size_t max, int *last)
{
    unsigned ch = sim_channels(s);
    int running = !!(s->status & SLOGIC_STATUS_RUNNING);
    size_t n = 0;

    *last = 0;
    while (n + 8 <= max) {
        /* Packer: fill 32 bits, two 32-bit words per 64-bit data word. */
        uint32_t w[2];
        int words = 0, flush = 0;
        for (int k = 0; k < 2; k++) {
            while (running && s->pack_bits < 32) {
                if (s->limit && s->sample >= s->limit) {
                    running = 0;
                    break;
                }
                if (s->overflow_after_words >= 0 && s->words_sent >= (uint64_t)s->overflow_after_words) {
                    running = 0;
                    s->status |= SLOGIC_STATUS_OVERFLOW;
                    break;
                }
                s->pack |= (uint64_t)(sim_sample(s, s->sample) & ((1u << ch) - 1)) << s->pack_bits;
                s->pack_bits += ch;
                s->sample++;
                s->count++;
            }
            if (s->pack_bits >= 32) {
                w[k] = (uint32_t)s->pack;
                s->pack >>= 32;
                s->pack_bits -= 32;
                words++;
                s->words_sent++;
            } else {
                /* Flushed last word (padded, possibly empty). */
                w[k] = (uint32_t)s->pack;
                s->pack = 0;
                s->pack_bits = 0;
                words++;
                flush = 1;
                break;
            }
        }
        for (int k = 0; k < 2; k++) {
            uint32_t v = k < words ? w[k] : 0;
            for (int b = 0; b < 4; b++)
                out[n + 4*k + b] = (uint8_t)(v >> (8*b));
        }
        n += 8;
        if (flush) {
            *last = 1;
            s->status &= ~SLOGIC_STATUS_RUNNING;
            s->status |= SLOGIC_STATUS_DONE;
            break;
        }
    }
    return n;
}

/* Bus */
/*-----*/

static uint32_t sim_read(struct sim *s, uint32_t addr)
{
    if (addr >= SLOGIC_FLASH_BUFFER_BASE && addr < SLOGIC_FLASH_BUFFER_BASE + SLOGIC_FLASH_BUFFER_SIZE) {
        uint32_t o = addr - SLOGIC_FLASH_BUFFER_BASE;
        return s->rx[o] | s->rx[o + 1] << 8 | s->rx[o + 2] << 16 | (uint32_t)s->rx[o + 3] << 24;
    }
    if (addr >= SLOGIC_IDENTIFIER_BASE && addr < SLOGIC_IDENTIFIER_BASE + 4*SLOGIC_IDENTIFIER_SIZE) {
        size_t i = (addr - SLOGIC_IDENTIFIER_BASE)/4;
        return i < sizeof(SIM_IDENT) ? (uint8_t)SIM_IDENT[i] : 0;
    }
    switch (addr) {
    case SLOGIC_ANALYZER_MAGIC:     return SLOGIC_MAGIC;
    case SLOGIC_ANALYZER_VERSION:   return 0x00000001;
    case SLOGIC_ANALYZER_CONFIG:    return 16 | SIM_RATIO << 8 | (SIM_FIFO_WORDS/64) << 16;
    case SLOGIC_ANALYZER_BASE_RATE: return (uint32_t)SIM_BASE_RATE;
    case SLOGIC_ANALYZER_CONTROL:   return s->control;
    case SLOGIC_ANALYZER_DIVIDER:   return s->divider;
    case SLOGIC_ANALYZER_LIMIT:     return (uint32_t)(s->limit >> 32);
    case SLOGIC_ANALYZER_LIMIT + 4: return (uint32_t)s->limit;
    case SLOGIC_ANALYZER_STATUS:    return s->status;
    case SLOGIC_ANALYZER_COUNT:     return (uint32_t)(s->count >> 32);
    case SLOGIC_ANALYZER_COUNT + 4: return (uint32_t)s->count;
    case SLOGIC_ANALYZER_CLK_COUNT: return (uint32_t)((slogic_time() - s->t0)*SIM_SAMPLE_CLK);
    case SLOGIC_FLASH_CONTROL:      return s->flash_control;
    case SLOGIC_FLASH_DIVIDER:      return s->flash_divider;
    case SLOGIC_FLASH_XFER:         return s->flash_xfer;
    case SLOGIC_FLASH_STATUS:       return 0;
    case SLOGIC_USB3_PHY_STATUS:    return SLOGIC_USB3_PHY_U0 | SLOGIC_USB3_PHY_READY | 0x0b;
    default:                        return 0;
    }
}

static void sim_write(struct sim *s, uint32_t addr, uint32_t val)
{
    if (addr >= SLOGIC_FLASH_BUFFER_BASE && addr < SLOGIC_FLASH_BUFFER_BASE + SLOGIC_FLASH_BUFFER_SIZE) {
        uint32_t o = addr - SLOGIC_FLASH_BUFFER_BASE;
        for (int b = 0; b < 4; b++)
            s->tx[o + b] = (uint8_t)(val >> (8*b));
        return;
    }
    switch (addr) {
    case SLOGIC_ANALYZER_CONTROL: {
        int rising  = (val & SLOGIC_CONTROL_ENABLE) && !(s->control & SLOGIC_CONTROL_ENABLE);
        int falling = !(val & SLOGIC_CONTROL_ENABLE) && (s->control & SLOGIC_CONTROL_ENABLE);
        s->control = val;
        if (rising)
            sim_start(s);
        if (falling)
            s->status &= ~SLOGIC_STATUS_RUNNING; /* Flushed by the generator (last). */
        break;
    }
    case SLOGIC_ANALYZER_DIVIDER:   s->divider = val; break;
    case SLOGIC_ANALYZER_LIMIT:     s->limit = (s->limit & 0xffffffffULL) | (uint64_t)val << 32; break;
    case SLOGIC_ANALYZER_LIMIT + 4: s->limit = (s->limit & ~0xffffffffULL) | val; break;
    case SLOGIC_FLASH_CONTROL:
        s->flash_control = val;
        if (!(val & SLOGIC_FLASH_CONTROL_ENABLE))
            sim_flash_deselect(s);
        break;
    case SLOGIC_FLASH_DIVIDER: s->flash_divider = val; break;
    case SLOGIC_FLASH_XFER:    s->flash_xfer = val; break;
    case SLOGIC_FLASH_CTRL:
        if (val & SLOGIC_FLASH_CTRL_START)
            sim_flash_xfer(s);
        if (val & SLOGIC_FLASH_CTRL_RELEASE)
            sim_flash_deselect(s);
        break;
    }
}

static int sim_read_burst(void *ctx, uint32_t addr, uint32_t *vals, size_t count)
{
    for (size_t i = 0; i < count; i++)
        vals[i] = sim_read(ctx, addr + 4*i);
    return SLOGIC_OK;
}

static int sim_write_burst(void *ctx, uint32_t addr, const uint32_t *vals, size_t count)
{
    for (size_t i = 0; i < count; i++)
        sim_write(ctx, addr + 4*i, vals[i]);
    return SLOGIC_OK;
}

/* Stream */
/*--------*/

static int sim_stream_start(void *ctx)
{
    struct sim *s = ctx;
    s->streaming = 1;
    s->ended     = 0;
    return SLOGIC_OK;
}

static int sim_stream_read(void *ctx, const uint8_t **data, size_t *size, int *last, int timeout_ms)
{
    struct sim *s = ctx;
    if (!s->streaming || s->ended)
        return SLOGIC_ERR_END;
    size_t max = SIM_BUF_SIZE;
    if (s->realtime && (s->status & SLOGIC_STATUS_RUNNING)) {
        /* Data paced at the samplerate: bytes available since the start. */
        double rate  = (double)SIM_BASE_RATE/(s->divider ? s->divider : 1)*sim_channels(s)/8;
        double t0    = slogic_time();
        for (;;) {
            double avail = (slogic_time() - s->t_start)*rate - (double)s->words_sent*4;
            if (avail >= 8) {
                max = avail < SIM_BUF_SIZE ? ((size_t)avail)/8*8 : SIM_BUF_SIZE;
                break;
            }
            if (!(s->control & SLOGIC_CONTROL_ENABLE) || (s->limit && s->sample >= s->limit))
                break;
            if ((slogic_time() - t0)*1e3 >= (timeout_ms < 0 ? 1e9 : timeout_ms))
                return SLOGIC_ERR_TIMEOUT;
            slogic_sleep_ms(1);
        }
    }
    *size = sim_generate(s, s->buf, max, last);
    *data = s->buf;
    if (*last)
        s->ended = 1;
    return SLOGIC_OK;
}

static int sim_stream_stop(void *ctx)
{
    struct sim *s = ctx;
    s->streaming = 0;
    return SLOGIC_OK;
}

/* Open/Close */
/*------------*/

static void sim_close(void *ctx)
{
    struct sim *s = ctx;
    if (!s)
        return;
    if (s->flash_writes_golden)
        fprintf(stderr, "slogic sim: %d write(s) to the golden image area!\n", s->flash_writes_golden);
    free(s->flash.mem);
    free(s);
}

static void sim_get_info(void *ctx, struct slogic_info *info)
{
    (void)ctx;
    snprintf(info->transport, sizeof(info->transport), "sim");
    snprintf(info->serial, sizeof(info->serial), "sim");
    info->usb_speed = 5000;
}

static const struct slogic_transport sim_transport = {
    .name         = "sim",
    .read_burst   = sim_read_burst,
    .write_burst  = sim_write_burst,
    .stream_start = sim_stream_start,
    .stream_read  = sim_stream_read,
    .stream_stop  = sim_stream_stop,
    .close        = sim_close,
    .get_info     = sim_get_info,
};

int slogic_sim_open(const char *args, struct slogic_dev *dev)
{
    struct sim *s = calloc(1, sizeof(*s));
    const char *p;

    if (!s)
        return SLOGIC_ERR_NOMEM;
    s->flash.mem = malloc(SIM_FLASH_SIZE);
    if (!s->flash.mem) {
        free(s);
        return SLOGIC_ERR_NOMEM;
    }
    memset(s->flash.mem, 0xff, SLOGIC_FLASH_APP_SIZE);
    memset(s->flash.mem + SLOGIC_FLASH_APP_SIZE, 0xa5, SIM_FLASH_SIZE - SLOGIC_FLASH_APP_SIZE);
    s->divider = 1;
    s->status  = SLOGIC_STATUS_DONE;
    s->t0      = slogic_time();
    s->pins[0] = 2; s->pins[1] = 0; s->pins[2] = 3; s->pins[3] = 1; /* Hidden mapping (probed). */
    if ((p = strstr(args, "rate=")))
        s->rate = atof(p + 5);
    s->realtime = !strstr(args, "fast");
    if ((p = strstr(args, "pins=")) && strlen(p + 5) >= 4)
        for (int i = 0; i < 4; i++)
            s->pins[i] = (unsigned)(p[5 + i] - '0') % SLOGIC_FLASH_NPADS;
    dev->t   = &sim_transport;
    dev->ctx = s;
    return SLOGIC_OK;
}

/* Test access: golden image area writes. */
int slogic_sim_golden_writes(struct slogic_dev *dev);
int slogic_sim_golden_writes(struct slogic_dev *dev)
{
    if (!dev || dev->t != &sim_transport)
        return -1;
    return ((struct sim *)dev->ctx)->flash_writes_golden;
}
