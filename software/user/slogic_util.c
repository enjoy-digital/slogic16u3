/* SPDX-License-Identifier: BSD-2-Clause
 *
 * slogic_util: SLogic16U3 device utility (info, registers, self-test, throughput, Flash/update, DFU).
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

/* Self-test: counter pattern captures over all channel modes/samplerates, data checked. */
static int cmd_check(uint64_t samples)
{
    struct slogic_dev *dev = open_dev();
    struct slogic_info info;
    uint64_t rates[64];
    int failures = 0, tests = 0;

    slogic_get_info(dev, &info);
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
                                       .pattern = SLOGIC_PATTERN_COUNTER };
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
                bad += v != ((k*divider) & 0xffff & ((1u << ch) - 1));
            }
            struct slogic_status st;
            slogic_capture_stop(dev, &st);
            int ok = (r == SLOGIC_ERR_END) && got == n && !bad && !st.overflow;
            printf("%2u ch ", ch);
            print_rate("@ ", rates[i]);
            printf(": %s (%llu samples, %llu errors%s)\n", ok ? "OK" : "FAIL", (unsigned long long)got,
                   (unsigned long long)bad, st.overflow ? ", overflow" : "");
            failures += !ok;
            tests++;
            free(s);
            free(u);
        }
    }
    printf("%d/%d OK\n", tests - failures, tests);
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
        printf("Updating application (%zu bytes at 0x%06x)...\n", size, SLOGIC_FLASH_APP_BASE);
        r = slogic_flash_update(dev, image, size, progress, NULL);
        printf("%s\n", r ? slogic_strerror(r) : "Done: replug the analyzer to boot the new gateware.");
        free(image);
    }
    slogic_flash_close(dev);
    slogic_close(dev);
    return r ? 1 : 0;
}

static int cmd_dfu(int argc, char **argv)
{
    struct slogic_dfu *dfu;
    uint8_t jedec[3];
    int r;

    if (argc < 1) {
        fprintf(stderr, "dfu id|dump <file>|update <image.bin>\n");
        return 1;
    }
    if ((r = slogic_dfu_open(&dfu, jedec))) {
        fprintf(stderr, "DFU loader (%04x:%04x, MODE button): %s\n", SLOGIC_DFU_VID, SLOGIC_DFU_PID,
                slogic_strerror(r));
        return 1;
    }
    printf("DFU loader: Flash JEDEC ID %02x %02x %02x\n", jedec[0], jedec[1], jedec[2]);
    if (!strcmp(argv[0], "dump") && argc >= 2) {
        size_t size = 16*1024*1024;
        uint8_t *data = malloc(size);
        FILE *f = fopen(argv[1], "wb");
        printf("Dumping the whole Flash (16MB, keep it outside the repository)...\n");
        r = (!f || slogic_dfu_read(dfu, 0, data, size) || fwrite(data, 1, size, f) != size);
        if (f)
            fclose(f);
        free(data);
    } else if (!strcmp(argv[0], "update") && argc >= 2) {
        size_t size;
        uint8_t *image = load_file(argv[1], &size);
        r = slogic_dfu_update(dfu, image, size, progress, NULL);
        printf("%s\n", r ? slogic_strerror(r) : "Done: replug the analyzer.");
        free(image);
    }
    slogic_dfu_close(dfu);
    return r ? 1 : 0;
}

/* Main */
/*------*/

static void help(void)
{
    printf("slogic_util " SLOGIC_VERSION_STRING ": SLogic16U3 device utility.\n"
           "usage: slogic_util [-d uri] <command> [args]\n"
           "  -d uri               Device: usb (default), usb:serial=<s>, sim (simulated device).\n"
           "commands:\n"
           "  info                 Device information.\n"
           "  status               Capture status.\n"
           "  reg <addr> [val]     Read/write a 32-bit register.\n"
           "  clock                Sample clock frequency (vs host clock).\n"
           "  check [samples]      Self-test: counter pattern captures, all modes/samplerates.\n"
           "  bench [ch] [MHz] [s] Throughput (continuous capture).\n"
           "  flash probe          Flash pads mapping probe, JEDEC ID.\n"
           "  flash read <f> <a> <n>  Read Flash to a file.\n"
           "  flash update <img>   Update the application (0x000000, the DFU loader is never written).\n"
           "  dfu id|dump <f>|update <img>  Sipeed DFU loader (MODE button): stock/recovery.\n");
}

int main(int argc, char **argv)
{
    int c;

    while ((c = getopt(argc, argv, "d:h")) != -1) {
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
    if (!strcmp(argv[0], "check"))  return cmd_check(argc > 1 ? strtoull(argv[1], NULL, 0) : 100000);
    if (!strcmp(argv[0], "bench"))
        return cmd_bench(argc > 1 ? atoi(argv[1]) : 16, (uint64_t)((argc > 2 ? atof(argv[2]) : 100)*1e6),
                         argc > 3 ? atof(argv[3]) : 5.0);
    if (!strcmp(argv[0], "flash"))  return cmd_flash(argc - 1, argv + 1);
    if (!strcmp(argv[0], "dfu"))    return cmd_dfu(argc - 1, argv + 1);
    help();
    return 1;
}
