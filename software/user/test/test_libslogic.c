/* SPDX-License-Identifier: BSD-2-Clause
 *
 * libslogic unit tests on the simulated device: info, configurations/samplerates, captures (counter
 * pattern: data checked sample by sample, limits, continuous/stop, overflow), unpacking, Flash (probe,
 * update, region guard).
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
#include "slogic_regs.h"

int slogic_sim_golden_writes(struct slogic_dev *dev);

static int failures = 0;
static int checks   = 0;

#define CHECK(cond) do { \
    checks++; \
    if (!(cond)) { \
        failures++; \
        fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #cond); \
    } \
} while (0)

#define CHECK_OK(expr) do { int _r = (expr); CHECK(_r == SLOGIC_OK); \
    if (_r) fprintf(stderr, "  -> %s\n", slogic_strerror(_r)); } while (0)

/* Helpers */
/*---------*/

static struct slogic_dev *open_sim(const char *uri)
{
    struct slogic_dev *dev = NULL;
    CHECK_OK(slogic_open(uri, &dev));
    return dev;
}

/* Capture all the data of a limited capture, returns the bytes (packed). */
static uint8_t *capture(struct slogic_dev *dev, const struct slogic_config *cfg, size_t *size, int *err)
{
    size_t cap = slogic_packed_bytes(cfg->channels, cfg->limit ? cfg->limit : 1000000) + 1024*1024;
    uint8_t *out = malloc(cap);
    const uint8_t *data;
    size_t n, total = 0;
    int r;

    *err = slogic_capture_start(dev, cfg);
    if (*err)
        return out;
    while ((r = slogic_capture_read(dev, &data, &n, 1000)) == SLOGIC_OK) {
        if (total + n > cap)
            break;
        memcpy(out + total, data, n);
        total += n;
    }
    *err  = r == SLOGIC_ERR_END ? SLOGIC_OK : r;
    *size = total;
    return out;
}

static void check_pattern(unsigned channels, uint64_t divider, const uint8_t *data, uint64_t nsamples)
{
    unsigned unit = slogic_unitsize(channels);
    uint8_t *s = malloc(nsamples*unit);
    uint32_t mask = (1u << channels) - 1;
    uint64_t bad = 0;

    slogic_unpack(channels, data, 0, nsamples, s);
    for (uint64_t i = 0; i < nsamples; i++) {
        uint32_t v = unit == 2 ? (uint32_t)(s[2*i] | s[2*i + 1] << 8) : s[i];
        if (v != ((i*divider) & 0xffff & mask))
            bad++;
    }
    CHECK(bad == 0);
    free(s);
}

/* Tests */
/*-------*/

static void test_info(void)
{
    struct slogic_dev *dev = open_sim("sim:fast");
    struct slogic_info info;

    CHECK_OK(slogic_get_info(dev, &info));
    CHECK(info.magic == SLOGIC_MAGIC);
    CHECK(info.nchannels == 16);
    CHECK(info.ratio == 8);
    CHECK(info.base_rate == 800000000ULL);
    CHECK(strstr(info.ident, "SLogic16U3") != NULL);
    CHECK(!strcmp(info.transport, "sim"));

    double freq;
    CHECK_OK(slogic_measure_sample_clock(dev, 50, &freq));
    CHECK(freq > 95e6 && freq < 105e6);
    slogic_close(dev);
}

static void test_config(void)
{
    struct slogic_dev *dev = open_sim("sim:fast");
    struct slogic_info info;
    struct slogic_config c = { .channels = 16, .samplerate = 200000000 };
    char msg[128];

    slogic_get_info(dev, &info);
    CHECK(slogic_config_check(&info, &c, msg, sizeof(msg)) == SLOGIC_OK);
    c.samplerate = 400000000;
    CHECK(slogic_config_check(&info, &c, msg, sizeof(msg)) == SLOGIC_ERR_ARG);
    CHECK(strstr(msg, "too high") != NULL);
    c.channels = 8;
    CHECK(slogic_config_check(&info, &c, NULL, 0) == SLOGIC_OK);
    c.channels = 4; c.samplerate = 800000000;
    CHECK(slogic_config_check(&info, &c, NULL, 0) == SLOGIC_OK);
    c.samplerate = 300000000; /* Not 800MHz/N. */
    CHECK(slogic_config_check(&info, &c, NULL, 0) == SLOGIC_ERR_ARG);
    c.channels = 5; c.samplerate = 1000000;
    CHECK(slogic_config_check(&info, &c, NULL, 0) == SLOGIC_ERR_ARG);
    c.channels = 16; c.samplerate = 160000000; /* /5: 2 samples per cycle. */
    CHECK(slogic_config_check(&info, &c, NULL, 0) == SLOGIC_OK);

    CHECK(slogic_max_samplerate(&info, 4)  == 800000000ULL);
    CHECK(slogic_max_samplerate(&info, 8)  == 400000000ULL);
    CHECK(slogic_max_samplerate(&info, 16) == 200000000ULL);

    uint64_t rates[64];
    int n = slogic_samplerates(&info, 16, rates, 64);
    CHECK(n > 10);
    CHECK(rates[n - 1] == 200000000ULL);
    for (int i = 1; i < n; i++)
        CHECK(rates[i] > rates[i - 1]);
    for (int i = 0; i < n; i++) {
        struct slogic_config cc = { .channels = 16, .samplerate = rates[i] };
        CHECK(slogic_config_check(&info, &cc, NULL, 0) == SLOGIC_OK);
    }
    n = slogic_samplerates(&info, 4, rates, 64);
    CHECK(rates[n - 1] == 800000000ULL);
    slogic_close(dev);
}

static void test_capture_patterns(void)
{
    static const struct { unsigned ch; uint64_t rate; uint64_t limit; } cases[] = {
        { 16, 200000000,  100000 },
        { 16, 160000000,   12345 },
        {  8, 400000000,   77777 },
        {  4, 800000000,    1001 },
        {  4,   1000000,  300001 },
        { 16,    100000,  600000 }, /* Several 256KB buffers. */
    };
    struct slogic_dev *dev = open_sim("sim:fast");
    struct slogic_info info;
    slogic_get_info(dev, &info);

    for (size_t i = 0; i < sizeof(cases)/sizeof(cases[0]); i++) {
        struct slogic_config c = { .channels = cases[i].ch, .samplerate = cases[i].rate,
                                   .limit = cases[i].limit, .pattern = SLOGIC_PATTERN_COUNTER };
        struct slogic_status st;
        size_t size = 0;
        int err;
        uint8_t *data = capture(dev, &c, &size, &err);
        CHECK(err == SLOGIC_OK);
        CHECK(size == slogic_packed_bytes(c.channels, c.limit));
        check_pattern(c.channels, info.base_rate/c.samplerate, data, c.limit);
        CHECK_OK(slogic_capture_stop(dev, &st));
        CHECK(st.done);
        CHECK(!st.overflow);
        CHECK(st.samples == c.limit);
        free(data);
    }
    slogic_close(dev);
}

/* Pattern generator: the signals decoded from a 16ch@100MHz capture (1 sample per 100MHz cycle). */
static void test_generator(void)
{
    struct slogic_dev *dev = open_sim("sim:fast");
    struct slogic_config c = { .channels = 16, .samplerate = 100000000, .limit = 200000,
                               .pattern = SLOGIC_PATTERN_GENERATOR };
    struct slogic_status st;
    size_t size = 0;
    int err;
    uint8_t *data = capture(dev, &c, &size, &err);
    uint16_t *v = malloc(c.limit*2);
    CHECK(err == SLOGIC_OK);
    CHECK_OK(slogic_capture_stop(dev, &st));
    slogic_unpack(16, data, 0, c.limit, (uint8_t *)v);

    /* Model: every sample. */
    uint64_t bad = 0;
    for (uint64_t i = 0; i < c.limit; i++)
        bad += v[i] != slogic_generator_sample(8*i);
    CHECK(bad == 0);

    /* CH0: UART 1Mbaud (100 samples per bit), sampled mid-bit from each start bit. */
    char text[64] = { 0 };
    size_t nt = 0;
    unsigned bad_stop = 0;
    CHECK(v[0] & 1); /* Idle at the capture start. */
    for (uint64_t i = 1; i + 1000 < c.limit && nt < sizeof(text) - 1; i++) {
        if ((v[i - 1] & 1) && !(v[i] & 1)) {
            uint8_t ch = 0;
            for (int b = 0; b < 8; b++)
                ch |= (v[i + 150 + 100*b] & 1) << b;
            bad_stop += !(v[i + 950] & 1); /* Stop bit. */
            text[nt++] = (char)ch;
            i += 950;
        }
    }
    CHECK(bad_stop == 0);
    CHECK(!strncmp(text, SLOGIC_GENERATOR_TEXT, strlen(SLOGIC_GENERATOR_TEXT)));

    /* CH1-CH3: SPI mode 0 (MOSI sampled on SCK rising edges while CS# low): "SLOG" frames. */
    unsigned frames = 0, nbits = 0, bad_frames = 0;
    uint32_t word = 0;
    for (uint64_t i = 1; i < c.limit; i++) {
        unsigned cs = (v[i] >> 1) & 1, sck = (v[i] >> 2) & 1, sck0 = (v[i - 1] >> 2) & 1;
        if (!cs && sck && !sck0) {
            word = word << 1 | ((v[i] >> 3) & 1);
            nbits++;
        }
        if (cs && !((v[i - 1] >> 1) & 1)) { /* CS# rising: end of frame. */
            bad_frames += !(nbits == 32 && word == 0x534c4f47);
            frames++;
            nbits = 0;
            word  = 0;
        }
    }
    CHECK(frames > 300);
    CHECK(bad_frames == 0);

    /* CH5: 100MHz clock sampled at 100MHz (samples 0, 8, ...: low). CH6-CH15: Gray counter, +1 per cycle. */
    uint64_t bad_clk = 0, bad_gray = 0;
    for (uint64_t i = 1; i < c.limit; i++) {
        unsigned b0 = 0, b1 = 0;
        for (unsigned g = v[i - 1] >> 6; g; g >>= 1) b0 ^= g; /* Gray to binary. */
        for (unsigned g = v[i] >> 6; g; g >>= 1) b1 ^= g;
        bad_clk  += (v[i] >> 5) & 1;
        bad_gray += b1 != ((b0 + 1) & 0x3ff);
    }
    CHECK(bad_clk == 0);
    CHECK(bad_gray == 0);
    free(v);
    free(data);
    slogic_close(dev);
}

/* Update image check: GW5AT-15 bitstream header, application region size. */
static void test_image_check(void)
{
    static const uint8_t hdr[] = { 0xff, 0xff, 0xff, 0xff, 0xa5, 0xc3, 0x06, 0x00, 0x00, 0x00,
                                   0x00, 0x01, 0x68, 0x1b, 0x10, 0x00 };
    uint8_t *img = malloc(SLOGIC_FLASH_SIZE);
    char msg[160];

    memset(img, 0xff, SLOGIC_FLASH_SIZE);
    memcpy(img + 16, hdr, sizeof(hdr));
    CHECK(slogic_image_check(img, 600000, msg, sizeof(msg)) == SLOGIC_OK);
    CHECK(slogic_image_check(img, SLOGIC_FLASH_SIZE, msg, sizeof(msg)) == SLOGIC_ERR_ARG); /* Full dump. */
    CHECK(strstr(msg, "dump") != NULL);
    CHECK(slogic_image_check(img, 0, NULL, 0) == SLOGIC_ERR_ARG);
    img[16 + 13] = 0x1c;                                                                   /* Other IDCODE. */
    CHECK(slogic_image_check(img, 600000, msg, sizeof(msg)) == SLOGIC_ERR_ARG);
    CHECK(strstr(msg, "IDCODE") != NULL);
    memset(img, 0x00, 4096);                                                               /* No header. */
    CHECK(slogic_image_check(img, 600000, msg, sizeof(msg)) == SLOGIC_ERR_ARG);
    free(img);
}

static void test_continuous_stop(void)
{
    struct slogic_dev *dev = open_sim("sim:fast");
    struct slogic_config c = { .channels = 16, .samplerate = 100000000, .pattern = SLOGIC_PATTERN_COUNTER };
    struct slogic_status st;
    const uint8_t *data;
    size_t n, total = 0;
    uint8_t *all = malloc(8*1024*1024);

    CHECK_OK(slogic_capture_start(dev, &c));
    for (int i = 0; i < 8; i++) {
        CHECK_OK(slogic_capture_read(dev, &data, &n, 1000));
        memcpy(all + total, data, n);
        total += n;
    }
    CHECK_OK(slogic_capture_stop(dev, &st));
    CHECK(total >= 8*128*1024);
    check_pattern(16, 8, all, total/2);
    free(all);
    slogic_close(dev);
}

static void test_overflow(void)
{
    /* Emulated host at 100MB/s, 16 channels at 200MS/s (400MB/s): overflow. */
    struct slogic_dev *dev = open_sim("sim:rate=100e6,fast");
    struct slogic_config c = { .channels = 16, .samplerate = 200000000, .limit = 10000000 };
    struct slogic_status st;
    size_t size;
    int err;
    uint8_t *data = capture(dev, &c, &size, &err);

    CHECK(err == SLOGIC_OK || err == SLOGIC_ERR_OVERFLOW);
    CHECK_OK(slogic_capture_status(dev, &st));
    CHECK(st.overflow);
    CHECK(st.samples < c.limit);
    free(data);
    slogic_close(dev);
}

static void test_unpack(void)
{
    uint8_t packed[4] = { 0x21, 0x43, 0x65, 0x87 }, out[8];

    slogic_unpack(4, packed, 0, 8, out);
    for (int i = 0; i < 8; i++)
        CHECK(out[i] == i + 1);
    /* From an odd sample index: data starts at the byte holding sample `first`. */
    slogic_unpack(4, packed, 1, 3, out);
    CHECK(out[0] == 2 && out[1] == 3 && out[2] == 4);
    CHECK(slogic_unitsize(4) == 1 && slogic_unitsize(8) == 1 && slogic_unitsize(16) == 2);
    CHECK(slogic_packed_bytes(4, 3) == 2 && slogic_packed_bytes(16, 3) == 6);
}

static void test_flash(void)
{
    struct slogic_dev *dev = open_sim("sim:fast");
    struct slogic_flash_pins pins;
    uint8_t jedec[3];
    size_t size = 300*1024 + 123;
    uint8_t *image = malloc(size), *check = malloc(size);

    for (size_t i = 0; i < size; i++)
        image[i] = (uint8_t)(i*13 + (i >> 8));

    /* Probe: hidden simulated mapping found (clk=2, cs_n=0, mosi=3, miso=1). */
    CHECK_OK(slogic_flash_probe(dev, &pins, jedec));
    CHECK(pins.clk == 2 && pins.cs_n == 0 && pins.mosi == 3 && pins.miso == 1);
    CHECK(jedec[0] == 0xef && jedec[1] == 0x40 && jedec[2] == 0x18);

    /* Update and read back. */
    CHECK_OK(slogic_flash_update(dev, image, size, NULL, NULL));
    CHECK_OK(slogic_flash_read(dev, 0, check, size));
    CHECK(!memcmp(image, check, size));

    /* Region guard: nothing written at or above 0x800000. */
    CHECK(slogic_flash_erase(dev, 0x7ff000, 8192) == SLOGIC_ERR_ARG);
    CHECK(slogic_flash_write(dev, 0x7fff00, image, 512) == SLOGIC_ERR_ARG);
    CHECK(slogic_flash_erase(dev, 0x800000, 4096) == SLOGIC_ERR_ARG);
    CHECK(slogic_sim_golden_writes(dev) == 0);
    uint8_t golden[16];
    CHECK_OK(slogic_flash_read(dev, 0x800000, golden, sizeof(golden)));
    CHECK(golden[0] == 0xa5 && golden[15] == 0xa5);

    /* Opened with known pins. */
    CHECK_OK(slogic_flash_close(dev));
    CHECK_OK(slogic_flash_open(dev, &pins));
    free(image);
    free(check);
    slogic_close(dev);
}

int main(void)
{
    test_info();
    test_config();
    test_capture_patterns();
    test_generator();
    test_image_check();
    test_continuous_stop();
    test_overflow();
    test_unpack();
    test_flash();
    printf("libslogic tests: %d checks, %d failure(s)\n", checks, failures);
    return failures ? 1 : 0;
}
