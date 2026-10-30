/* SPDX-License-Identifier: BSD-2-Clause
 *
 * slogic16u3_record: SLogic16U3 capture to a file (raw packed samples, unpacked binary, VCD or CSV).
 *
 *   slogic16u3_record -r 100M -c 16 -n 1000000 -o capture.vcd
 *   slogic16u3_record -r 800M -c 4 -t 2 -o capture.bin        (raw: as received, see slogic_unpack)
 *
 * This file is part of SLogic16U3.
 *
 * Copyright (c) 2026 Enjoy-Digital <enjoy-digital.fr>
 *
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include <getopt.h>

#include "slogic.h"
#include "slogic_regs.h"

enum { FMT_RAW, FMT_U8, FMT_VCD, FMT_CSV };

static volatile int stop_requested = 0;

static void on_sigint(int sig)
{
    (void)sig;
    stop_requested = 1;
}

static uint64_t parse_rate(const char *s)
{
    char *end;
    double v = strtod(s, &end);
    if (*end == 'k' || *end == 'K') v *= 1e3;
    if (*end == 'M')                v *= 1e6;
    if (*end == 'G')                v *= 1e9;
    return (uint64_t)(v + 0.5);
}

static int format_from_name(const char *path)
{
    const char *ext = strrchr(path, '.');
    if (ext && !strcmp(ext, ".vcd")) return FMT_VCD;
    if (ext && !strcmp(ext, ".csv")) return FMT_CSV;
    if (ext && !strcmp(ext, ".u8"))  return FMT_U8;
    return FMT_RAW;
}

/* VCD/CSV writers (changes only for VCD). */
struct writer {
    FILE    *f;
    int      fmt;
    unsigned channels;
    uint64_t period_ps;
    uint64_t index;
    uint32_t last;
};

static void writer_header(struct writer *w)
{
    if (w->fmt == FMT_VCD) {
        fprintf(w->f, "$timescale 1 ps $end\n$scope module slogic16u3 $end\n");
        for (unsigned i = 0; i < w->channels; i++)
            fprintf(w->f, "$var wire 1 %c CH%u $end\n", '!' + i, i);
        fprintf(w->f, "$upscope $end\n$enddefinitions $end\n");
        w->last = 0xffffffff;
    } else if (w->fmt == FMT_CSV) {
        fprintf(w->f, "time_ps");
        for (unsigned i = 0; i < w->channels; i++)
            fprintf(w->f, ",CH%u", i);
        fprintf(w->f, "\n");
    }
}

static void writer_samples(struct writer *w, const uint8_t *u, uint64_t n)
{
    unsigned unit = slogic_unitsize(w->channels);
    for (uint64_t k = 0; k < n; k++, w->index++) {
        uint32_t v = unit == 2 ? (uint32_t)(u[2*k] | u[2*k + 1] << 8) : u[k];
        if (w->fmt == FMT_VCD) {
            if (v == w->last)
                continue;
            fprintf(w->f, "#%llu\n", (unsigned long long)(w->index*w->period_ps));
            for (unsigned i = 0; i < w->channels; i++)
                if (((v ^ w->last) >> i) & 1)
                    fprintf(w->f, "%u%c\n", (v >> i) & 1, '!' + i);
            w->last = v;
        } else {
            fprintf(w->f, "%llu", (unsigned long long)(w->index*w->period_ps));
            for (unsigned i = 0; i < w->channels; i++)
                fprintf(w->f, ",%u", (v >> i) & 1);
            fprintf(w->f, "\n");
        }
    }
}

static void help(void)
{
    printf("slogic16u3_record " SLOGIC_VERSION_STRING ": SLogic16U3 capture to a file.\n"
           "usage: slogic16u3_record [options] -o <file>\n"
           "  -d uri       Device (usb, usb:serial=<s>, sim).\n"
           "  -r rate      Samplerate (ex: 100M, default 100M).\n"
           "  -c channels  4, 8 or 16 (default 16).\n"
           "  -n samples   Samples to capture (default: 1M, 0: until Ctrl-C/-t).\n"
           "  -t seconds   Capture duration (continuous capture).\n"
           "  -p           Counter test pattern instead of the inputs.\n"
           "  -g           Pattern generator (UART, SPI, PWM, clock, Gray counter) instead of the inputs.\n"
           "  -o file      Output: .vcd, .csv, .u8 (unpacked: 1/2 bytes per sample), else raw packed.\n");
}

int main(int argc, char **argv)
{
    struct slogic_config cfg = { .samplerate = 100000000, .channels = 16, .limit = 1000000 };
    const char *uri = NULL, *out = NULL;
    double duration = 0;
    struct slogic_dev *dev;
    struct slogic_info info;
    struct slogic_status st;
    char msg[160];
    int c, r;

    while ((c = getopt(argc, argv, "d:r:c:n:t:pgo:h")) != -1) {
        switch (c) {
        case 'd': uri = optarg; break;
        case 'r': cfg.samplerate = parse_rate(optarg); break;
        case 'c': cfg.channels = atoi(optarg); break;
        case 'n': cfg.limit = strtoull(optarg, NULL, 0); break;
        case 't': duration = atof(optarg); cfg.limit = 0; break;
        case 'p': cfg.pattern = SLOGIC_PATTERN_COUNTER; break;
        case 'g': cfg.pattern = SLOGIC_PATTERN_GENERATOR; break;
        case 'o': out = optarg; break;
        default:  help(); return 1;
        }
    }
    if (!out) {
        help();
        return 1;
    }
    if ((r = slogic_open(uri, &dev))) {
        fprintf(stderr, "Could not open the device: %s\n", slogic_strerror(r));
        return 1;
    }
    slogic_get_info(dev, &info);
    if (slogic_config_check(&info, &cfg, msg, sizeof(msg))) {
        fprintf(stderr, "Invalid configuration: %s\n", msg);
        slogic_close(dev);
        return 1;
    }

    struct writer w = { .fmt = format_from_name(out), .channels = cfg.channels,
                        .period_ps = (uint64_t)(1e12/cfg.samplerate + 0.5) };
    w.f = fopen(out, "wb");
    if (!w.f) {
        perror(out);
        slogic_close(dev);
        return 1;
    }
    writer_header(&w);
    signal(SIGINT, on_sigint);

    if ((r = slogic_capture_start(dev, &cfg))) {
        fprintf(stderr, "Capture start: %s\n", slogic_strerror(r));
        return 1;
    }
    const uint8_t *data;
    size_t size;
    uint64_t bytes = 0, samples = 0;
    uint8_t *u = NULL;
    size_t u_size = 0;
    double t0 = slogic_time();
    for (;;) {
        if (stop_requested || (duration > 0 && slogic_time() - t0 >= duration))
            break;
        r = slogic_capture_read(dev, &data, &size, 1000);
        if (r == SLOGIC_ERR_TIMEOUT)
            continue;
        if (r)
            break;
        uint64_t n = size*8/cfg.channels;
        if (w.fmt == FMT_RAW) {
            fwrite(data, 1, size, w.f);
        } else {
            if (n*2 > u_size) {
                u_size = n*2;
                u = realloc(u, u_size);
            }
            slogic_unpack(cfg.channels, data, samples, n, u);
            if (w.fmt == FMT_U8)
                fwrite(u, slogic_unitsize(cfg.channels), n, w.f);
            else
                writer_samples(&w, u, n);
        }
        bytes   += size;
        samples += n;
    }
    double dt = slogic_time() - t0;
    slogic_capture_stop(dev, &st);
    fclose(w.f);
    free(u);
    if (cfg.limit && samples > cfg.limit)
        samples = cfg.limit;
    fprintf(stderr, "%llu samples (%u channels @ %g MHz), %.1f MB in %.2fs (%.1f MB/s)%s%s\n",
            (unsigned long long)samples, cfg.channels, cfg.samplerate/1e6, bytes/1e6, dt, bytes/dt/1e6,
            st.overflow ? ", OVERFLOW: data lost (lower the samplerate/channels)" : "",
            (r && r != SLOGIC_ERR_END) ? ", stopped on error" : "");
    slogic_close(dev);
    return st.overflow ? 2 : 0;
}
