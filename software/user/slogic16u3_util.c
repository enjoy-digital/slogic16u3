/* SPDX-License-Identifier: BSD-2-Clause
 *
 * slogic16u3_util: SLogic16U3 device utility (info, registers, self-test, throughput, Flash/update, DFU).
 *
 * This file is part of SLogic16U3.
 *
 * Copyright (c) 2026 Enjoy-Digital <enjoy-digital.fr>
 *
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <getopt.h>

#include "slogic.h"
#include "slogic_dfu.h"
#include "slogic_regs.h"

static const char *uri = NULL;

/* Helpers */
/*---------*/

static struct slogic_dev *open_dev(void)
{
    struct slogic_dev *dev;
    int r = slogic_open(uri, &dev);
    if (r) {
        fprintf(stderr, "Could not open %s: %s\n", uri ? uri : "usb", slogic_strerror(r));
        exit(1);
    }
    return dev;
}

static uint8_t *load_file(const char *path, size_t *size)
{
    FILE *f = fopen(path, "rb");
    uint8_t *data;
    long n;

    if (!f) {
        perror(path);
        exit(1);
    }
    fseek(f, 0, SEEK_END);
    n = ftell(f);
    fseek(f, 0, SEEK_SET);
    data = malloc(n > 0 ? n : 1);
    if (!data || fread(data, 1, n, f) != (size_t)n) {
        fprintf(stderr, "%s: read error\n", path);
        exit(1);
    }
    fclose(f);
    *size = n;
    return data;
}

/* Image check before an update (GW5AT-15 bitstream, application region). */
static int image_ok(const uint8_t *image, size_t size)
{
    char msg[160];
    if (slogic_image_check(image, size, msg, sizeof(msg))) {
        fprintf(stderr, "Image refused: %s\n", msg);
        return 0;
    }
    return 1;
}

static void progress(const char *step, size_t done, size_t total, void *user)
{
    (void)user;
    fprintf(stderr, "\r  %-8s %3d%% (%zu/%zu)", step, total ? (int)(100.0*done/total) : 100, done, total);
    if (done == total)
        fprintf(stderr, "\n");
}

static void print_rate(const char *label, uint64_t rate)
{
    if (rate >= 1000000)
        printf("%s%g MHz", label, rate/1e6);
    else
        printf("%s%g kHz", label, rate/1e3);
}

/* Commands */
/*----------*/

static int cmd_info(void)
{
    struct slogic_dev *dev = open_dev();
    struct slogic_info info;
    uint32_t phy;

    slogic_get_info(dev, &info);
    slogic_read32(dev, SLOGIC_USB3_PHY_STATUS, &phy);
    printf("Device     : %s\n", info.ident);
    printf("Transport  : %s (serial %s, %d Mb/s)\n", info.transport, info.serial, info.usb_speed);
    printf("Gateware   : v%u.%u\n", info.version >> 16, info.version & 0xffff);
    printf("Channels   : %u\n", info.nchannels);
    print_rate("Base rate  : ", info.base_rate);
    printf(" (%u samples/cycle)\n", info.ratio);
    printf("FIFO       : %u x 64-bit words\n", info.fifo_words);
    printf("USB3 PHY   : LTSSM 0x%02x%s%s\n", SLOGIC_USB3_PHY_LTSSM(phy),
           (phy & SLOGIC_USB3_PHY_U0) ? ", U0" : "", (phy & SLOGIC_USB3_PHY_READY) ? ", SerDes ready" : "");
    for (unsigned ch = 4; ch <= 16; ch *= 2) {
        printf("Max rate   : %2u channels: ", ch);
        print_rate("", slogic_max_samplerate(&info, ch));
        printf("\n");
    }
    slogic_close(dev);
    return 0;
}

static int cmd_reg(int argc, char **argv)
{
    struct slogic_dev *dev;
    uint32_t addr, val;

    if (argc < 1) {
        fprintf(stderr, "reg <addr> [value]\n");
        return 1;
    }
    dev  = open_dev();
    addr = strtoul(argv[0], NULL, 0);
    if (argc > 1) {
        val = strtoul(argv[1], NULL, 0);
        slogic_write32(dev, addr, val);
    }
    slogic_read32(dev, addr, &val);
    printf("0x%08x: 0x%08x\n", addr, val);
    slogic_close(dev);
    return 0;
}

static int cmd_status(void)
{
    struct slogic_dev *dev = open_dev();
    struct slogic_status st;
    slogic_capture_status(dev, &st);
    printf("running %d, done %d, overflow %d, error %d, samples %llu\n", st.running, st.done,
           st.overflow, st.error, (unsigned long long)st.samples);
    slogic_close(dev);
    return 0;
}

static int cmd_clock(void)
{
    struct slogic_dev *dev = open_dev();
    struct slogic_info info;
    double freq;
    slogic_get_info(dev, &info);
    if (slogic_measure_sample_clock(dev, 2000, &freq) == SLOGIC_OK) {
        double nominal = (double)info.base_rate/info.ratio;
        printf("Sample clock: %.0f Hz (nominal %.0f Hz, %+.1f ppm)\n", freq, nominal,
               (freq - nominal)/nominal*1e6);
    }
    slogic_close(dev);
    return 0;
}

/* Expected sample i (16-bit) of a test pattern capture. */
static uint16_t check_expected(unsigned pattern, uint64_t i, uint64_t divider)
{
    if (pattern == SLOGIC_PATTERN_GENERATOR)
        return slogic_generator_sample(i*divider);
    return (uint16_t)(i*divider);
}

/* Trigger reference (sigrok semantics) on the pattern generator: first matching sample index. */
static int check_trigger_match(const struct slogic_trigger *t, unsigned ch, uint16_t p, uint16_t v, int have_prev)
{
    uint16_t m = (uint16_t)((1u << ch) - 1), edges = (t->rising | t->falling) & m;
    if ((v & t->zero & m) || (~v & t->one & m))
        return 0;
    if (edges && (!have_prev || (edges & ~((t->rising & ~p & v) | (t->falling & p & ~v)))))
        return 0;
    return 1;
}

/* Hardware trigger self-test (pattern generator): trigger position, pre-trigger window and every sample
   checked against the model. Returns 1 if OK. */
static int check_trigger(struct slogic_dev *dev, const struct slogic_info *info, unsigned ch, uint64_t rate,
                         const char *cond)
{
    struct slogic_config c = { .channels = ch, .samplerate = rate, .limit = 10000,
                               .pattern = SLOGIC_PATTERN_GENERATOR, .trigger_enable = 1,
                               .pretrigger = slogic_max_pretrigger(info, ch)/2 };
    uint64_t divider = info->base_rate/rate, expected = 0, bad = 0, n;
    struct slogic_status st;
    const uint8_t *data;
    size_t size, total = 0, cap = slogic_packed_bytes(ch, c.limit + c.pretrigger + 64) + 64*1024;
    uint8_t *s = malloc(cap);
    int r, ok;

    slogic_trigger_parse(cond, &c.trigger);
    for (uint64_t i = 0; i < 100000000; i++)
        if (check_trigger_match(&c.trigger, ch, i ? slogic_generator_sample((i - 1)*divider) : 0,
                                slogic_generator_sample(i*divider), i > 0)) {
            expected = i;
            break;
        }
    if ((r = slogic_capture_start(dev, &c)) == SLOGIC_OK) {
        while ((r = slogic_capture_read(dev, &data, &size, 3000)) == SLOGIC_OK) {
            if (total + size > cap)
                size = cap - total;
            memcpy(s + total, data, size);
            total += size;
        }
    }
    slogic_capture_status(dev, &st);
    slogic_capture_stop(dev, NULL);
    n = total*8/ch;
    if (st.triggered && n > st.trigger_offset + c.limit)
        n = st.trigger_offset + c.limit; /* 4 channels: padding nibble of the last byte. */
    if (st.triggered && n == st.trigger_offset + c.limit) {
        unsigned unit = slogic_unitsize(ch);
        uint8_t *u = malloc(n*unit);
        uint64_t first = st.trigger_sample - st.trigger_offset;
        slogic_unpack(ch, s, 0, n, u);
        for (uint64_t k = 0; k < n; k++) {
            uint32_t v = unit == 2 ? (uint32_t)(u[2*k] | u[2*k + 1] << 8) : u[k];
            bad += v != (slogic_generator_sample((first + k)*divider) & ((1u << ch) - 1));
        }
        free(u);
    }
    ok = r == SLOGIC_ERR_END && st.triggered && st.trigger_sample == expected &&
         st.trigger_offset >= (c.pretrigger < expected ? c.pretrigger : expected) &&
         n == st.trigger_offset + c.limit && !bad && !st.overflow;
    printf("trigger   %2u ch ", ch);
    print_rate("@ ", rate);
    printf(": %s (%s: sample %llu, expected %llu, %llu pre-trigger, %llu samples, %llu errors%s)\n",
           ok ? "OK" : "FAIL", cond, (unsigned long long)st.trigger_sample, (unsigned long long)expected,
           (unsigned long long)st.trigger_offset, (unsigned long long)n, (unsigned long long)bad,
           st.overflow ? ", overflow" : "");
    free(s);
    return ok;
}

/* Self-test: test pattern captures (counter and/or pattern generator) over all channel
   modes/samplerates, every sample checked, hardware trigger (gateware >= 0.2). */
static int cmd_check(uint64_t samples, const char *which)
{
    struct slogic_dev *dev = open_dev();
    struct slogic_info info;
    uint64_t rates[64];
    unsigned patterns[2], npatterns = 0;
    int failures = 0, tests = 0, triggers = !strcmp(which, "trigger") || !strcmp(which, "all");

    if (!strcmp(which, "counter") || !strcmp(which, "all"))
        patterns[npatterns++] = SLOGIC_PATTERN_COUNTER;
    if (!strcmp(which, "generator") || !strcmp(which, "all"))
        patterns[npatterns++] = SLOGIC_PATTERN_GENERATOR;
    if (!npatterns && !triggers) {
        fprintf(stderr, "check: unknown pattern %s (counter, generator, trigger, all)\n", which);
        return 1;
    }
    slogic_get_info(dev, &info);
    for (unsigned p = 0; p < npatterns; p++)
    for (unsigned ch = 4; ch <= 16; ch *= 2) {
        int n = slogic_samplerates(&info, ch, rates, 64);
        for (int i = 0; i < n; i++) {
            /* Captures up to ~0.1s (low rates: at least 1000 samples), read timeout from the capture
               duration (a Bulk IN transfer completes when full or on the final short packet). */
            uint64_t n = samples;
            if (n > rates[i]/10)
                n = rates[i]/10 > 1000 ? rates[i]/10 : 1000;
            int timeout_ms = 2000 + (int)(2000*n/rates[i]);
            struct slogic_config c = { .channels = ch, .samplerate = rates[i], .limit = n,
                                       .pattern = patterns[p] };
            uint64_t divider = info.base_rate/rates[i], bad = 0, got = 0;
            unsigned unit = slogic_unitsize(ch);
            uint8_t *s = malloc(slogic_packed_bytes(ch, n) + 16), *u = malloc(n*unit);
            const uint8_t *data;
            size_t size, total = 0;
            int r;

            if ((r = slogic_capture_start(dev, &c)) == SLOGIC_OK) {
                while ((r = slogic_capture_read(dev, &data, &size, timeout_ms)) == SLOGIC_OK) {
                    memcpy(s + total, data, size);
                    total += size;
                }
            }
            got = total*8/ch;
            if (got > n)
                got = n;
            slogic_unpack(ch, s, 0, got, u);
            for (uint64_t k = 0; k < got; k++) {
                uint32_t v = unit == 2 ? (uint32_t)(u[2*k] | u[2*k + 1] << 8) : u[k];
                bad += v != (check_expected(patterns[p], k, divider) & ((1u << ch) - 1));
            }
            struct slogic_status st;
            slogic_capture_stop(dev, &st);
            int ok = (r == SLOGIC_ERR_END) && got == n && !bad && !st.overflow;
            printf("%-9s %2u ch ", patterns[p] == SLOGIC_PATTERN_GENERATOR ? "generator" : "counter", ch);
            print_rate("@ ", rates[i]);
            printf(": %s (%llu samples, %llu errors%s)\n", ok ? "OK" : "FAIL", (unsigned long long)got,
                   (unsigned long long)bad, st.overflow ? ", overflow" : "");
            failures += !ok;
            tests++;
            free(s);
            free(u);
        }
    }
    if (triggers && !slogic_has_trigger(&info)) {
        printf("trigger: not supported by this gateware (0.2 or later), skipped\n");
    } else if (triggers) {
        /* Gray counter (CH6-CH15) = gray(700) with the 100MHz clock (CH5) high. */
        static const char *gray700 = "CH5=1,CH6=0,CH7=1,CH8=0,CH9=0,CH10=0,CH11=1,CH12=1,CH13=1,CH14=1,CH15=1";
        static const char *conds[] = { "CH1=f", "CH0=f", "CH3=e", "CH1=0,CH2=r" };
        for (unsigned ch = 4; ch <= 16; ch *= 2) {
            uint64_t max = slogic_max_samplerate(&info, ch);
            uint64_t sel[3] = { max, max/4, 1000000 };
            for (int i = 0; i < 3; i++) {
                if (info.base_rate % sel[i])
                    continue;
                for (unsigned k = 0; k < sizeof(conds)/sizeof(conds[0]); k++) {
                    failures += !check_trigger(dev, &info, ch, sel[i], conds[k]);
                    tests++;
                }
            }
            if (ch == 16) {
                /* Early pattern, late pattern (with the PWM high: cycle 176*1024 + 700, full pre-trigger). */
                /* (CH5: 100MHz clock, never high with the samples on the cycle starts below max/2.) */
                char late[160];
                snprintf(late, sizeof(late), "%s,CH4=1", gray700);
                failures += !check_trigger(dev, &info, ch, max, gray700);
                failures += !check_trigger(dev, &info, ch, max, late);
                failures += !check_trigger(dev, &info, ch, max/4, late + strlen("CH5=1,"));
                tests += 3;
            }
        }
    }
    printf("%d/%d OK\n", tests - failures, tests);
    slogic_close(dev);
    return failures ? 1 : 0;
}

/* Outputs */
/*---------*/

static int cmd_out(int argc, char **argv)
{
    struct slogic_dev *dev = open_dev();
    struct slogic_info info;
    char name[16], src[16];
    int r = 0;

    slogic_get_info(dev, &info);
    unsigned n = slogic_output_count(&info);
    if (!n) {
        fprintf(stderr, "out: no outputs on this gateware (0.3 or later)\n");
        slogic_close(dev);
        return 1;
    }
    if (argc >= 5 && !strcmp(argv[0], "timing")) {
        r = slogic_output_timing(dev, (uint32_t)strtoul(argv[1], NULL, 0), (uint32_t)strtoul(argv[2], NULL, 0),
                                 (uint32_t)strtoul(argv[3], NULL, 0), (uint32_t)strtoul(argv[4], NULL, 0));
    } else if (argc >= 2 && !strcmp(argv[0], "all") && !strcmp(argv[1], "off")) {
        r = slogic_outputs_off(dev);
    } else if (argc >= 2) {
        int out = slogic_output_find(&info, argv[0]);
        struct slogic_output o = { 0 };
        if (out < 0) {
            fprintf(stderr, "out: unknown output %s\n", argv[0]);
            r = 1;
        } else if (!strcmp(argv[1], "off")) {
            slogic_output_get(dev, (unsigned)out, &o);
            o.enable = 0;
            r = slogic_output_set(dev, (unsigned)out, &o);
        } else if (slogic_output_source_parse(argv[1], &o.source)) {
            fprintf(stderr, "out: unknown source %s (gpio, trigger, armed, capture, periodic, gen0-gen7)\n", argv[1]);
            r = 1;
        } else {
            o.enable = 1;
            for (int i = 2; i < argc; i++) {
                if (!strcmp(argv[i], "inv"))
                    o.invert = 1;
                else
                    o.level = atoi(argv[i]);
            }
            r = slogic_output_set(dev, (unsigned)out, &o);
        }
    } else if (argc != 0) {
        fprintf(stderr, "out [<output> off|<source> [level] [inv] | all off | timing <width> <delay> <period> <high>]\n");
        r = 1;
    }
    if (!r) {
        uint32_t levels = 0;
        slogic_output_levels(dev, &levels);
        for (unsigned i = 0; i < n; i++) {
            struct slogic_output o;
            slogic_output_get(dev, i, &o);
            slogic_output_name(&info, i, name, sizeof(name));
            printf("%-5s %-8s %-9s%s%s pad %u\n", name, o.enable ? "driven" : "tristate",
                   slogic_output_source_name(o.source, src, sizeof(src)), o.invert ? " inv" : "",
                   o.source == SLOGIC_OUT_GPIO ? (o.level ? " 1" : " 0") : "", (levels >> i) & 1);
        }
        if (argc >= 5 && !strcmp(argv[0], "timing"))
            printf("timing (sample clock cycles, %.0fns): trigger pulse width %s delay %s, periodic period %s high %s\n",
                   1e9*info.ratio/info.base_rate, argv[1], argv[2], argv[3], argv[4]);
    } else if (r < 0) {
        fprintf(stderr, "out: %s\n", slogic_strerror(r));
    }
    slogic_close(dev);
    return r ? 1 : 0;
}

/* Loopback self-test: pattern generator channels on an output, wired to an input channel: captured
 * input checked against the generator model (latency searched), real input path validated. */
static int cmd_loopback(const char *out_name, const char *ch_name)
{
    static const unsigned gens[] = { 0, 2 }; /* UART (1Mbaud), SPI SCK (12.5MHz). */
    struct slogic_dev *dev = open_dev();
    struct slogic_info info;
    int out, ch = -1, failures = 0;

    slogic_get_info(dev, &info);
    out = slogic_output_find(&info, out_name);
    if (ch_name && (ch_name[0] == 'C' || ch_name[0] == 'c'))
        ch = atoi(ch_name + 2);
    if (out < 0 || ch < 0 || ch > 15) {
        fprintf(stderr, "loopback <output> <channel> (ex: loopback AUX0 CH0): unknown output/channel\n");
        slogic_close(dev);
        return 1;
    }
    unsigned channels = ch < 4 ? 4 : ch < 8 ? 8 : 16;
    uint64_t n = 200000, rate = info.base_rate/info.ratio; /* One sample per sample clock cycle. */
    for (unsigned g = 0; g < sizeof(gens)/sizeof(gens[0]); g++) {
        struct slogic_output o = { .enable = 1, .source = SLOGIC_OUT_GENERATOR + gens[g] };
        struct slogic_config c = { .channels = channels, .samplerate = rate, .limit = n };
        uint8_t *s = malloc(slogic_packed_bytes(channels, n) + 16), *u = malloc(2*n);
        const uint8_t *data;
        size_t size, total = 0;
        int r;
        slogic_output_set(dev, (unsigned)out, &o);
        if ((r = slogic_capture_start(dev, &c)) == SLOGIC_OK)
            while ((r = slogic_capture_read(dev, &data, &size, 2000)) == SLOGIC_OK) {
                memcpy(s + total, data, size);
                total += size;
            }
        slogic_capture_stop(dev, NULL);
        uint64_t got = total*8/channels > n ? n : total*8/channels, best = got + 1, edges = 0;
        int best_d = -1;
        slogic_unpack(channels, s, 0, got, u);
        unsigned unit = slogic_unitsize(channels);
        for (int d = 0; d < 32; d++) {
            uint64_t bad = 0;
            for (uint64_t i = 0; i < got; i++) {
                uint32_t v = unit == 2 ? (uint32_t)(u[2*i] | u[2*i + 1] << 8) : u[i];
                uint64_t gc = i >= (uint64_t)d ? i - d : 0;
                bad += ((v >> ch) & 1) != ((slogic_generator_sample(8*gc) >> gens[g]) & 1u);
            }
            if (bad < best) {
                best   = bad;
                best_d = d;
            }
        }
        for (uint64_t i = 1; i < got; i++) {
            uint32_t a = unit == 2 ? (uint32_t)(u[2*i] | u[2*i + 1] << 8) : u[i];
            uint32_t b = unit == 2 ? (uint32_t)(u[2*i - 2] | u[2*i - 1] << 8) : u[i - 1];
            edges += ((a ^ b) >> ch) & 1;
        }
        int ok = r == SLOGIC_ERR_END && got == n && best == 0 && edges > 0;
        printf("loopback %s -> CH%d gen%u: %s (%llu samples, %llu edges, %llu errors, latency %d cycles)\n",
               out_name, ch, gens[g], ok ? "OK" : "FAIL", (unsigned long long)got, (unsigned long long)edges,
               (unsigned long long)(best > got ? got : best), best_d);
        failures += !ok;
        free(s);
        free(u);
    }
    struct slogic_output off = { 0 };
    slogic_output_set(dev, (unsigned)out, &off);
    slogic_close(dev);
    return failures ? 1 : 0;
}

/* Throughput: continuous capture, highest samplerate without overflow. */
static int cmd_bench(unsigned channels, uint64_t samplerate, double duration)
{
    struct slogic_dev *dev = open_dev();
    struct slogic_config c = { .channels = channels, .samplerate = samplerate };
    struct slogic_status st;
    const uint8_t *data;
    size_t size;
    uint64_t total = 0;
    int r;

    if ((r = slogic_capture_start(dev, &c))) {
        fprintf(stderr, "start: %s\n", slogic_strerror(r));
        return 1;
    }
    double t0 = slogic_time();
    while (slogic_time() - t0 < duration) {
        if ((r = slogic_capture_read(dev, &data, &size, 1000)))
            break;
        total += size;
    }
    double dt = slogic_time() - t0;
    slogic_capture_stop(dev, &st);
    printf("%u channels @ %g MHz: %.1f MB/s over %.1fs%s\n", channels, samplerate/1e6, total/dt/1e6, dt,
           st.overflow ? " (OVERFLOW: data lost)" : "");
    slogic_close(dev);
    return st.overflow ? 1 : 0;
}

static int cmd_flash(int argc, char **argv)
{
    struct slogic_dev *dev;
    struct slogic_flash_pins pins;
    uint8_t jedec[3];
    int r;

    if (argc < 1) {
        fprintf(stderr, "flash probe|read <file> <addr> <size>|update <image.bin>\n");
        return 1;
    }
    dev = open_dev();
    if ((r = slogic_flash_probe(dev, &pins, jedec))) {
        fprintf(stderr, "Flash probe: %s\n", slogic_strerror(r));
        slogic_close(dev);
        return 1;
    }
    printf("Flash: JEDEC ID %02x %02x %02x, pads clk=%u cs_n=%u mosi=%u miso=%u\n",
           jedec[0], jedec[1], jedec[2], pins.clk, pins.cs_n, pins.mosi, pins.miso);
    if (!strcmp(argv[0], "read") && argc >= 4) {
        uint32_t addr = strtoul(argv[2], NULL, 0);
        size_t size   = strtoul(argv[3], NULL, 0);
        uint8_t *data = malloc(size);
        FILE *f = fopen(argv[1], "wb");
        if (!f || slogic_flash_read(dev, addr, data, size) || fwrite(data, 1, size, f) != size)
            r = 1;
        if (f)
            fclose(f);
        free(data);
    } else if (!strcmp(argv[0], "update") && argc >= 2) {
        size_t size;
        uint8_t *image = load_file(argv[1], &size);
        if (!image || !image_ok(image, size)) {
            free(image);
            slogic_flash_close(dev);
            slogic_close(dev);
            return 1;
        }
        printf("Updating application (%zu bytes at 0x%06x)...\n", size, SLOGIC_FLASH_APP_BASE);
        r = slogic_flash_update(dev, image, size, progress, NULL);
        printf("%s\n", r ? slogic_strerror(r) : "Done: replug the analyzer to boot the new gateware.");
        free(image);
    }
    slogic_flash_close(dev);
    slogic_close(dev);
    return r ? 1 : 0;
}

/* Used 64KB blocks of a Flash dump. */
static void print_blocks(const uint8_t *data, size_t size)
{
    size_t start = 0;
    int used = 0;
    for (size_t o = 0; o <= size; o += 65536) {
        int u = 0;
        for (size_t i = o; o < size && i < o + 65536 && !u; i++)
            u = data[i] != 0xff;
        if (u && !used)
            start = o;
        if (!u && used)
            printf("  used 0x%06zx-0x%06zx (%zu KB)%s\n", start, o - 1, (o - start)/1024,
                   start >= SLOGIC_FLASH_APP_SIZE ? " DFU loader" : " application");
        used = u;
    }
}

/* Sipeed DFU loader (MODE button): backup, install, restore. The DFU loader (0x800000-) is never written. */
static int cmd_dfu(int argc, char **argv)
{
    struct slogic_dfu *dfu;
    uint8_t jedec[3];
    double t0 = slogic_time();
    int r = 1;

    if (argc < 1 || (strcmp(argv[0], "id") && argc < 2)) {
        fprintf(stderr, "dfu id | dump <backup.bin> | update <image.bin> --backup <backup.bin> | restore <backup.bin>\n");
        return 1;
    }
    if ((r = slogic_dfu_open(&dfu, jedec))) {
        fprintf(stderr, "DFU loader (%04x:%04x, MODE button: LED blinking red): %s\n", SLOGIC_DFU_VID,
                SLOGIC_DFU_PID, slogic_strerror(r));
        return 1;
    }
    printf("DFU loader: Flash JEDEC ID %02x %02x %02x\n", jedec[0], jedec[1], jedec[2]);
    if (!strcmp(argv[0], "id")) {
        r = 0;
    } else if (!strcmp(argv[0], "dump")) {
        /* Read twice and compared: the backup is the way back to the stock firmware. */
        uint8_t *data = malloc(SLOGIC_FLASH_SIZE), *again = malloc(SLOGIC_FLASH_SIZE);
        FILE *f = NULL;
        printf("Dumping the whole Flash (16MB, read twice)...\n");
        r = !data || !again ||
            slogic_dfu_read(dfu, 0, data, SLOGIC_FLASH_SIZE, progress, NULL) ||
            slogic_dfu_read(dfu, 0, again, SLOGIC_FLASH_SIZE, progress, NULL);
        if (!r && memcmp(data, again, SLOGIC_FLASH_SIZE)) {
            fprintf(stderr, "The two reads differ: retry the dump.\n");
            r = 1;
        }
        if (!r && !(f = fopen(argv[1], "wb")))
            r = 1;
        if (!r)
            r = fwrite(data, 1, SLOGIC_FLASH_SIZE, f) != SLOGIC_FLASH_SIZE;
        if (f)
            r |= fclose(f) != 0;
        if (!r) {
            printf("%s: 16MB backup of this unit (keep it, it is the way back to the stock firmware).\n", argv[1]);
            print_blocks(data, SLOGIC_FLASH_SIZE);
        } else {
            fprintf(stderr, "Dump failed.\n");
        }
        free(data);
        free(again);
    } else if (!strcmp(argv[0], "update") || !strcmp(argv[0], "restore")) {
        /* The backup (full dump of this unit) is required: checked against the Flash's DFU loader. */
        int restore = !strcmp(argv[0], "restore");
        const char *bpath = restore ? argv[1] : (argc >= 4 && !strcmp(argv[2], "--backup") ? argv[3] : NULL);
        size_t bsize = 0, size = 0;
        uint8_t *backup = NULL, *image = NULL;
        char msg[160];

        if (!bpath) {
            fprintf(stderr, "dfu update needs --backup <backup.bin>: dump this unit first (dfu dump).\n");
            goto done;
        }
        if (!(backup = load_file(bpath, &bsize)))
            goto done;
        if ((r = slogic_dfu_check_backup(dfu, backup, bsize, msg, sizeof(msg)))) {
            fprintf(stderr, "Backup refused: %s\n", msg);
            r = 1;
            goto done;
        }
        if (restore) {
            /* Stock application: the backup's application region (trailing erased bytes dropped). */
            size = SLOGIC_FLASH_APP_SIZE;
            while (size && backup[size - 1] == 0xff)
                size--;
            if (!size) {
                fprintf(stderr, "No application in the backup.\n");
                r = 1;
                goto done;
            }
            image = backup;
            printf("Restoring the application from %s (%zu bytes)...\n", bpath, size);
        } else {
            if (!(image = load_file(argv[1], &size)) || !image_ok(image, size)) {
                r = 1;
                goto done;
            }
            printf("Installing %s (%zu bytes at 0x%06x)...\n", argv[1], size, SLOGIC_FLASH_APP_BASE);
        }
        r = slogic_dfu_update(dfu, image, size, progress, NULL);
        printf("%s\n", r ? slogic_strerror(r) : "Done (verified): replug the analyzer.");
done:
        if (image != backup)
            free(image);
        free(backup);
    } else {
        fprintf(stderr, "dfu: unknown command %s\n", argv[0]);
    }
    printf("(%.0fs)\n", slogic_time() - t0);
    slogic_dfu_close(dfu);
    return r ? 1 : 0;
}

/* Main */
/*------*/

static void help(void)
{
    printf("slogic16u3_util " SLOGIC_VERSION_STRING ": SLogic16U3 device utility.\n"
           "usage: slogic16u3_util [-d uri] <command> [args]\n"
           "  -d uri               Device: usb (default), usb:serial=<s>, sim (simulated device).\n"
           "commands:\n"
           "  info                 Device information.\n"
           "  status               Capture status.\n"
           "  reg <addr> [val]     Read/write a 32-bit register.\n"
           "  clock                Sample clock frequency (vs host clock).\n"
           "  check [samples] [counter|generator|trigger|all]\n"
           "                       Self-test: test pattern captures, all modes/samplerates.\n"
           "  bench [ch] [MHz] [s] Throughput (continuous capture).\n"
           "  out                  Outputs (AUX0/AUX1, output channels): state, pad levels.\n"
           "  out <o> <source> [level] [inv]  Drive an output: gpio, trigger, armed, capture, periodic, gen0-gen7.\n"
           "  out <o> off | out all off       Back to tristate.\n"
           "  out timing <width> <delay> <period> <high>  Timings (sample clock cycles).\n"
           "  loopback <o> <ch>    Output wired to an input channel: generator checked (input path self-test).\n"
           "  flash probe          Flash pads mapping probe, JEDEC ID.\n"
           "  flash read <f> <a> <n>  Read Flash to a file.\n"
           "  flash update <img>   Update the application (0x000000, the DFU loader is never written).\n"
           "  dfu id               Sipeed DFU loader (MODE button: LED blinking red): Flash ID.\n"
           "  dfu dump <backup>    Full 16MB backup of this unit (read twice): the way back to stock.\n"
           "  dfu update <img> --backup <backup>  Install a gateware image (backup checked first).\n"
           "  dfu restore <backup> Stock firmware back from the backup (the DFU loader is never written).\n");
}

int main(int argc, char **argv)
{
    int c;

    while ((c = getopt(argc, argv, "+d:h")) != -1) {
        switch (c) {
        case 'd': uri = optarg; break;
        default:  help(); return 1;
        }
    }
    argc -= optind;
    argv += optind;
    if (argc < 1) {
        help();
        return 1;
    }
    if (!strcmp(argv[0], "info"))   return cmd_info();
    if (!strcmp(argv[0], "status")) return cmd_status();
    if (!strcmp(argv[0], "reg"))    return cmd_reg(argc - 1, argv + 1);
    if (!strcmp(argv[0], "clock"))  return cmd_clock();
    if (!strcmp(argv[0], "check"))  return cmd_check(argc > 1 ? strtoull(argv[1], NULL, 0) : 100000,
                                                     argc > 2 ? argv[2] : "all");
    if (!strcmp(argv[0], "bench"))
        return cmd_bench(argc > 1 ? atoi(argv[1]) : 16, (uint64_t)((argc > 2 ? atof(argv[2]) : 100)*1e6),
                         argc > 3 ? atof(argv[3]) : 5.0);
    if (!strcmp(argv[0], "out"))    return cmd_out(argc - 1, argv + 1);
    if (!strcmp(argv[0], "loopback") && argc >= 3) return cmd_loopback(argv[1], argv[2]);
    if (!strcmp(argv[0], "flash"))  return cmd_flash(argc - 1, argv + 1);
    if (!strcmp(argv[0], "dfu"))    return cmd_dfu(argc - 1, argv + 1);
    help();
    return 1;
}
