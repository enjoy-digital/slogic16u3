/* SPDX-License-Identifier: BSD-2-Clause
 *
 * libslogic16u3: SLogic16U3 (LiteX gateware) host library.
 *
 * Device access over USB3 (EP0: SoC/CSR accesses, Bulk IN: capture data), or a simulated device
 * (software model of the gateware, for tests/development without hardware):
 *
 *   slogic_open("usb", &dev)  / slogic_open("usb:serial=0001", &dev) / slogic_open("sim", &dev)
 *   slogic_get_info(dev, &info)
 *   slogic_capture_start(dev, &config)
 *   while (slogic_capture_read(dev, &data, &size, timeout_ms) == SLOGIC_OK) ...
 *   slogic_capture_stop(dev, &status)
 *   slogic_close(dev)
 *
 * Capture data: sample-major packed samples, first sample on the LSBs (4 channels: 2 samples per byte,
 * 8 channels: 1 byte, 16 channels: 2 bytes little-endian per sample), see slogic_unpack().
 *
 * This file is part of SLogic16U3.
 *
 * Copyright (c) 2026 Enjoy-Digital <enjoy-digital.fr>
 *
 */

#ifndef SLOGIC_H
#define SLOGIC_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define SLOGIC_VERSION_STRING "0.1.0"

/* Errors */
/*--------*/

enum {
    SLOGIC_OK           =  0,
    SLOGIC_ERR          = -1,
    SLOGIC_ERR_ARG      = -2, /* Invalid argument/configuration. */
    SLOGIC_ERR_IO       = -3, /* Transport error. */
    SLOGIC_ERR_TIMEOUT  = -4,
    SLOGIC_ERR_NODEV    = -5, /* No device found. */
    SLOGIC_ERR_DEVICE   = -6, /* Unexpected device (magic/version). */
    SLOGIC_ERR_OVERFLOW = -7, /* Capture data lost (host too slow). */
    SLOGIC_ERR_END      = -8, /* End of the capture (no more data). */
    SLOGIC_ERR_NOMEM    = -9,
    SLOGIC_ERR_FLASH    = -10,
};

const char *slogic_strerror(int err);

/* Device */
/*--------*/

struct slogic_dev;

struct slogic_info {
    uint32_t magic;
    uint32_t version;          /* Gateware version: major[31:16], minor[15:0]. */
    unsigned nchannels;        /* Physical channels (16). */
    unsigned ratio;            /* Samples per sample clock cycle (8). */
    uint64_t base_rate;        /* Base samplerate (Hz): samplerates are base_rate/divider. */
    unsigned fifo_words;       /* Capture FIFO (64-bit words). */
    unsigned naux;             /* AUX IOs (outputs 0..naux-1, gateware >= 0.3). */
    uint16_t output_channels;  /* Output channels (mask, outputs naux.. in ascending order). */
    char     ident[256];       /* SoC identifier (with the build date). */
    char     transport[16];    /* "usb", "sim". */
    char     serial[64];       /* USB serial number. */
    int      usb_speed;        /* USB speed (Mb/s: 5000). */
};

/* uri: "usb[:serial=<s>][,path=<bus-ports>]", "sim" (NULL: "usb"). */
int  slogic_open(const char *uri, struct slogic_dev **dev);
void slogic_close(struct slogic_dev *dev);
int  slogic_get_info(struct slogic_dev *dev, struct slogic_info *info);

/* Raw SoC/CSR accesses (byte addresses, 32-bit words, see slogic_regs.h). */
int slogic_read32(struct slogic_dev *dev, uint32_t addr, uint32_t *val);
int slogic_write32(struct slogic_dev *dev, uint32_t addr, uint32_t val);
int slogic_read64(struct slogic_dev *dev, uint32_t addr, uint64_t *val);   /* 64-bit CSR (MSW first). */
int slogic_write64(struct slogic_dev *dev, uint32_t addr, uint64_t val);
int slogic_read_burst(struct slogic_dev *dev, uint32_t addr, uint32_t *vals, size_t count);
int slogic_write_burst(struct slogic_dev *dev, uint32_t addr, const uint32_t *vals, size_t count);

/* Monotonic host time (seconds). */
double slogic_time(void);

/* Sample clock frequency (Hz) measured against the host clock over duration_ms. */
int slogic_measure_sample_clock(struct slogic_dev *dev, unsigned duration_ms, double *freq);

/* Capture */
/*---------*/

/* Hardware trigger (gateware >= 0.2): single stage, bit n: CHn, all the channels ANDed (channels without
 * condition: don't care, rising + falling: any edge). */
struct slogic_trigger {
    uint16_t zero;             /* Channels low. */
    uint16_t one;              /* Channels high. */
    uint16_t rising;           /* Channels rising (previous sample low, this one high). */
    uint16_t falling;          /* Channels falling. */
};

struct slogic_config {
    uint64_t samplerate;       /* Hz: base_rate/divider. */
    unsigned channels;         /* 4, 8 or 16 (CH0..CHn-1). */
    uint64_t limit;            /* Samples (0: continuous, until slogic_capture_stop). With the trigger:
                                * samples from the trigger sample (included). */
    unsigned pattern;          /* 0: inputs, 1: counter test pattern, 2: pattern generator. */
    int      trigger_enable;   /* Hardware trigger: nothing is streamed until it matches. */
    struct slogic_trigger trigger;
    uint64_t pretrigger;       /* Samples kept before the trigger (<= slogic_max_pretrigger(), at least
                                * this many unless the trigger comes earlier). */
};

struct slogic_status {
    int      running;
    int      done;
    int      overflow;         /* Data lost (host too slow): capture stopped. */
    int      error;            /* Invalid configuration. */
    uint64_t samples;          /* Captured samples. */
    uint64_t bytes;            /* Bytes received by the host. */
    int      triggered;        /* Hardware trigger matched. */
    uint64_t trigger_sample;   /* Trigger sample index in the capture (since the start). */
    uint64_t trigger_offset;   /* Trigger sample index in the stream (samples returned by
                                * slogic_capture_read: the pre-trigger samples come first). */
};

/* Configuration check: SLOGIC_OK or SLOGIC_ERR_ARG (msg: reason, may be NULL). */
int slogic_config_check(const struct slogic_info *info, const struct slogic_config *config,
                        char *msg, size_t msg_size);
/* Supported samplerates for a channel count (ascending), returns the number of rates (<= max). */
int slogic_samplerates(const struct slogic_info *info, unsigned channels, uint64_t *rates, int max);
/* Maximum samplerate supported by the gateware datapath for a channel count. */
uint64_t slogic_max_samplerate(const struct slogic_info *info, unsigned channels);

/* Hardware trigger support (gateware >= 0.2) and maximum pre-trigger samples for a channel count. */
int      slogic_has_trigger(const struct slogic_info *info);
uint64_t slogic_max_pretrigger(const struct slogic_info *info, unsigned channels);
/* Trigger from a sigrok like string: "CH1=f,CH6=1,CH7=0,CH0=r,CH3=e" (0/1: level, r/f/e: rising/
 * falling/any edge), SLOGIC_OK or SLOGIC_ERR_ARG. */
int      slogic_trigger_parse(const char *s, struct slogic_trigger *trigger);

int slogic_capture_start(struct slogic_dev *dev, const struct slogic_config *config);
/* Next data buffer (valid until the next call): SLOGIC_OK, SLOGIC_ERR_TIMEOUT (no data yet: also while
 * waiting for the trigger), SLOGIC_ERR_END (capture done, all data read), SLOGIC_ERR_OVERFLOW or a
 * transport error. With the trigger, the status gives the trigger position once data is received. */
int slogic_capture_read(struct slogic_dev *dev, const uint8_t **data, size_t *size, int timeout_ms);
int slogic_capture_stop(struct slogic_dev *dev, struct slogic_status *status);
int slogic_capture_status(struct slogic_dev *dev, struct slogic_status *status);

/* Data */
/*------*/

/* Bytes per sample once unpacked (1 for 4/8 channels, 2 for 16 channels). */
unsigned slogic_unitsize(unsigned channels);
/* Bytes of packed data holding nsamples samples. */
uint64_t slogic_packed_bytes(unsigned channels, uint64_t nsamples);
/* Unpack nsamples samples from packed data (starting at sample index first of data, so streams can be
 * unpacked buffer by buffer) to unitsize bytes per sample. */
void slogic_unpack(unsigned channels, const uint8_t *data, uint64_t first, uint64_t nsamples, uint8_t *out);

/* Pattern generator (pattern 2, gateware/generator.py), sample t (base sample index from the capture
 * start, the capture's sample i is t = i*divider): CH0 UART 1Mbaud 8N1 (SLOGIC_GENERATOR_TEXT), CH1-CH3
 * SPI mode 0 12.5MHz (CS#, SCK, MOSI: "SLOG"), CH4 PWM ~97.7kHz (duty sweep), CH5 100MHz clock, CH6-CH15
 * 10-bit Gray counter at 100MHz. */
#define SLOGIC_GENERATOR_TEXT "SLogic16U3 LiteX pattern generator\r\n"
uint16_t slogic_generator_sample(uint64_t t);

/* Outputs (gateware 0.3) */
/*-------------------------*/

/* Tristate outputs (0: tristate until enabled): AUX0/AUX1 (JST SH connector UART pins, 3.3V), then the
 * output channels of the build (--outputs: ~1.2-1.5V at the connector through the input dividers). The
 * sources are evaluated on the sample clock (100MHz: 10ns), with fixed latencies (doc/GATEWARE.md). */
#define SLOGIC_OUT_GPIO       0 /* Level (struct slogic_output.level). */
#define SLOGIC_OUT_TRIGGER    1 /* Pulse (width cycles) delay cycles after the hardware trigger. */
#define SLOGIC_OUT_ARMED      2 /* High while waiting for the trigger. */
#define SLOGIC_OUT_CAPTURE    3 /* High while capturing. */
#define SLOGIC_OUT_PERIODIC   4 /* High `high` cycles every `period` cycles (restarted high on capture start). */
#define SLOGIC_OUT_GENERATOR  8 /* + k: pattern generator channel k (0-7, cycle rate). */

/* Gateware latencies (sample clock cycles, pads excluded, test/test_output.py): trigger sample cycle ->
 * trigger pulse (delay 0), generator cycle -> output. */
#define SLOGIC_OUT_TRIGGER_LATENCY   8
#define SLOGIC_OUT_GENERATOR_LATENCY 1
#define SLOGIC_OUT_PERIODIC_LATENCY  3 /* Capture start (sample 0 cycle) -> first periodic high cycle. */

struct slogic_output {
    int      enable;           /* Drive the pad (0: tristate). */
    unsigned source;           /* SLOGIC_OUT_*. */
    int      invert;
    int      level;            /* SLOGIC_OUT_GPIO level. */
};

unsigned slogic_output_count(const struct slogic_info *info);
/* Output name ("AUX0", "CH15") / index from a name (< 0: not an output), channel (< 0: AUX IO). */
int slogic_output_name(const struct slogic_info *info, unsigned out, char *name, size_t size);
int slogic_output_find(const struct slogic_info *info, const char *name);
int slogic_output_channel(const struct slogic_info *info, unsigned out);
/* Source from/to a name: gpio, trigger, armed, capture, periodic, gen0-gen7. */
int slogic_output_source_parse(const char *s, unsigned *source);
const char *slogic_output_source_name(unsigned source, char *buf, size_t size);

int slogic_output_set(struct slogic_dev *dev, unsigned out, const struct slogic_output *o);
int slogic_output_get(struct slogic_dev *dev, unsigned out, struct slogic_output *o);
/* Shared timings (sample clock cycles, write-only in the gateware): trigger pulse width (<= 255) and
 * delay (< 2^24), periodic period (1 - 2^24) and high time (<= period). */
int slogic_output_timing(struct slogic_dev *dev, uint32_t width, uint32_t delay, uint32_t period, uint32_t high);
int slogic_output_levels(struct slogic_dev *dev, uint32_t *levels); /* Pad levels (bit n: output n). */
int slogic_outputs_off(struct slogic_dev *dev);                       /* All tristate. */

/* Flash (configuration SPI Flash, remote update) */
/*-----------------------------------------------*/

#define SLOGIC_FLASH_SIZE        0x1000000 /* 16MB: application (0x000000) + Sipeed DFU loader (0x800000). */
#define SLOGIC_FLASH_APP_BASE    0x000000
#define SLOGIC_FLASH_APP_SIZE    0x800000 /* Application region (the DFU loader is above, never written). */
#define SLOGIC_FLASH_SECTOR      4096
#define SLOGIC_FLASH_PAGE        256

struct slogic_flash_pins {
    unsigned clk, cs_n, mosi, miso; /* Pad indexes (0-5: K13, K14, L14, L13, M14, M13). */
};

/* Find the pads mapping (JEDEC ID read with each mapping), jedec: 3 bytes (may be NULL). */
int slogic_flash_probe(struct slogic_dev *dev, struct slogic_flash_pins *pins, uint8_t jedec[3]);
int slogic_flash_open(struct slogic_dev *dev, const struct slogic_flash_pins *pins); /* NULL: default/probed. */
int slogic_flash_close(struct slogic_dev *dev);
int slogic_flash_jedec_id(struct slogic_dev *dev, uint8_t jedec[3]);
int slogic_flash_read(struct slogic_dev *dev, uint32_t addr, uint8_t *data, size_t size);
/* Erase/program in the application region only (addr + size <= SLOGIC_FLASH_APP_SIZE). */
int slogic_flash_erase(struct slogic_dev *dev, uint32_t addr, size_t size);
int slogic_flash_write(struct slogic_dev *dev, uint32_t addr, const uint8_t *data, size_t size);

typedef void (*slogic_progress_cb)(const char *step, size_t done, size_t total, void *user);
/* Application image check: a GW5AT-15 bitstream (sync word and SLOGIC_GW5AT15_IDCODE in its header) that fits
 * the application region (not a full Flash dump). SLOGIC_OK or SLOGIC_ERR_ARG (msg: reason, may be NULL). */
#define SLOGIC_GW5AT15_IDCODE 0x0001681b
int slogic_image_check(const uint8_t *image, size_t size, char *msg, size_t msg_size);
/* Application update: erase, program and verify image at 0x000000 (replug to boot it). */
int slogic_flash_update(struct slogic_dev *dev, const uint8_t *image, size_t size,
                        slogic_progress_cb progress, void *user);

#ifdef __cplusplus
}
#endif

#endif /* SLOGIC_H */
