/* SPDX-License-Identifier: BSD-2-Clause
 *
 * slogic16u3_scopehal_bridge: SLogic16U3 to ngscopeclient/libscopehal bridge (TwinLan transport).
 *
 *   slogic16u3_scopehal_bridge [-d uri] [--scpi-port 5025] [--waveform-port 5026]
 *   ngscopeclient: add instrument, driver "slogic", transport "twinlan", path "localhost:5025:5026".
 *
 * Control port: SCPI lines (libscopehal RemoteBridgeOscilloscope command set):
 *   *IDN? CHANS? ARMED? RATES? DEPTHS?  START SINGLE FORCE STOP  RATE <hz> DEPTH <n>
 *   TRIG:SOU <ch> TRIG:EDGE:DIR RISING|FALLING|ANY TRIG:DELAY <fs> TRIG:MODE EDGE TRIG:LEV <v>
 *   :<ch>:ON :<ch>:OFF :<ch>:THRESH <v> :<ch>:HYS <v>   (<ch>: 0-15)
 * Waveform port: the client sends 'K', the bridge replies with the next waveform (blocking until the
 * trigger, FORCE or STOP): u32 seqnum, u16 nchannels, i64 fs_per_sample, i64 trigger_fs, f64 wfms/s,
 * then per enabled channel: u64 channel, u64 bytes, i32 first_sample (0), bytes of bits (sample 8*m+b
 * on bit b of byte m).
 *
 * Triggers (software): edge on one channel with pre-trigger samples from TRIG:DELAY (time from the start
 * of the waveform to the trigger). Capture mode (4/8/16 channels) from the highest enabled channel.
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
#include <errno.h>
#include <signal.h>
#include <getopt.h>
#include <unistd.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <netinet/tcp.h>

#include "slogic.h"

#define FS_PER_SECOND 1e15

enum { EDGE_RISING, EDGE_FALLING, EDGE_ANY };
enum { ARM_STOPPED, ARM_CONTINUOUS, ARM_SINGLE };

/* State */
/*-------*/

struct bridge {
    struct slogic_dev *dev;
    struct slogic_info info;
    uint64_t rates[64];
    int      nrates;
    uint64_t rate;
    uint64_t depth;
    int      trig_src;      /* -1: none (free running). */
    int      trig_edge;
    int64_t  trig_delay_fs;
    uint32_t enabled;       /* Channels mask. */
    int      armed;
    int      force;
    uint32_t seqnum;
    int      scpi_fd, data_fd;
    char     line[1024];
    size_t   line_len;
    double   last_wfm_t;
    double   wfms;
};

static volatile int quit = 0;
static void on_signal(int sig) { (void)sig; quit = 1; }

/* Sockets */
/*---------*/

static int listen_on(int port)
{
    int fd = socket(AF_INET6, SOCK_STREAM, 0), one = 1, zero = 0;
    struct sockaddr_in6 a = { .sin6_family = AF_INET6, .sin6_port = htons(port), .sin6_addr = in6addr_any };

    if (fd < 0)
        return -1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
    setsockopt(fd, IPPROTO_IPV6, IPV6_V6ONLY, &zero, sizeof(zero));
    if (bind(fd, (struct sockaddr *)&a, sizeof(a)) || listen(fd, 1)) {
        close(fd);
        return -1;
    }
    return fd;
}

static int send_all(int fd, const void *buf, size_t len)
{
    const uint8_t *p = buf;
    while (len) {
        ssize_t n = send(fd, p, len, MSG_NOSIGNAL);
        if (n <= 0)
            return -1;
        p   += n;
        len -= n;
    }
    return 0;
}

static void reply(struct bridge *b, const char *s)
{
    if (b->scpi_fd >= 0 && (send_all(b->scpi_fd, s, strlen(s)) || send_all(b->scpi_fd, "\n", 1))) {
        close(b->scpi_fd);
        b->scpi_fd = -1;
    }
}

/* SCPI */
/*------*/

static int parse_channel(const char *s)
{
    char *end;
    long ch = strtol(s, &end, 10);
    return (*s && !*end && ch >= 0 && ch < 16) ? (int)ch : -1;
}

static unsigned capture_channels(const struct bridge *b)
{
    int highest = 0;
    for (int i = 0; i < 16; i++)
        if (b->enabled & (1u << i))
            highest = i;
    return highest < 4 ? 4 : highest < 8 ? 8 : 16;
}

static void scpi_line(struct bridge *b, char *line)
{
    char *args = strchr(line, ' ');
    char buf[4096];

    if (args) {
        *args++ = '\0';
        while (*args == ' ')
            args++;
    }
    /* Queries. */
    if (!strcasecmp(line, "*IDN?")) {
        snprintf(buf, sizeof(buf), "Sipeed,SLogic16U3 LiteX,%s,%u.%u", b->info.serial[0] ? b->info.serial : "0",
                 b->info.version >> 16, b->info.version & 0xffff);
        reply(b, buf);
    } else if (!strcasecmp(line, "CHANS?")) {
        reply(b, "0"); /* Analog channels. */
    } else if (!strcasecmp(line, "ARMED?")) {
        reply(b, b->armed ? "1" : "0");
    } else if (!strcasecmp(line, "RATES?")) {
        size_t n = 0;
        buf[0] = '\0';
        for (int i = 0; i < b->nrates; i++)
            n += snprintf(buf + n, sizeof(buf) - n, "%f,", FS_PER_SECOND/b->rates[i]);
        reply(b, buf);
    } else if (!strcasecmp(line, "DEPTHS?")) {
        reply(b, "1000,10000,100000,1000000,10000000,");
    }
    /* Commands. */
    else if (!strcasecmp(line, "START"))  { b->armed = ARM_CONTINUOUS; b->force = 0; }
    else if (!strcasecmp(line, "SINGLE")) { b->armed = ARM_SINGLE; b->force = 0; }
    else if (!strcasecmp(line, "FORCE"))  { b->armed = ARM_SINGLE; b->force = 1; }
    else if (!strcasecmp(line, "STOP"))   { b->armed = ARM_STOPPED; }
    else if (!strcasecmp(line, "RATE") && args) {
        uint64_t rate = strtoull(args, NULL, 10);
        if (rate && b->info.base_rate % rate == 0)
            b->rate = rate;
    } else if (!strcasecmp(line, "DEPTH") && args) {
        uint64_t depth = strtoull(args, NULL, 10);
        if (depth >= 8 && depth <= 100000000)
            b->depth = depth;
    } else if (!strcasecmp(line, "TRIG:SOU") && args) {
        b->trig_src = parse_channel(args);
    } else if (!strcasecmp(line, "TRIG:EDGE:DIR") && args) {
        b->trig_edge = !strcasecmp(args, "FALLING") ? EDGE_FALLING : !strcasecmp(args, "ANY") ? EDGE_ANY : EDGE_RISING;
    } else if (!strcasecmp(line, "TRIG:DELAY") && args) {
        b->trig_delay_fs = strtoll(args, NULL, 10);
    } else if (!strcasecmp(line, "TRIG:MODE") || !strcasecmp(line, "TRIG:LEV")) {
        /* Edge only / no threshold control (fixed input threshold). */
    } else if (line[0] == ':') {
        char *cmd = strchr(line + 1, ':');
        int ch;
        if (!cmd)
            return;
        *cmd++ = '\0';
        if ((ch = parse_channel(line + 1)) < 0)
            return;
        if (!strcasecmp(cmd, "ON"))
            b->enabled |= 1u << ch;
        else if (!strcasecmp(cmd, "OFF"))
            b->enabled &= ~(1u << ch);
    }
}

static void scpi_poll(struct bridge *b)
{
    char buf[512];
    ssize_t n;

    if (b->scpi_fd < 0)
        return;
    n = recv(b->scpi_fd, buf, sizeof(buf), MSG_DONTWAIT);
    if (n == 0 || (n < 0 && errno != EAGAIN && errno != EWOULDBLOCK)) {
        close(b->scpi_fd);
        b->scpi_fd = -1;
        b->armed   = ARM_STOPPED;
        return;
    }
    for (ssize_t i = 0; i < n; i++) {
        char c = buf[i];
        if (c == '\n' || c == ';') {
            b->line[b->line_len] = '\0';
            if (b->line_len)
                scpi_line(b, b->line);
            b->line_len = 0;
        } else if (c != '\r' && b->line_len < sizeof(b->line) - 1) {
            b->line[b->line_len++] = c;
        }
    }
}

/* Waveforms */
/*-----------*/

static int edge_match(int edge, int prev, int cur)
{
    switch (edge) {
    case EDGE_RISING:  return !prev && cur;
    case EDGE_FALLING: return prev && !cur;
    default:           return prev != cur;
    }
}

/* Capture one waveform (depth samples, 16-bit values), returns the pre-trigger samples count or < 0
 * (stopped/error). Control commands serviced while waiting for the trigger. */
/* Samplerate of the waveform: the requested one or, when too high for the enabled channels, the highest
 * supported one below (the waveform header gives the sample period). */
static uint64_t effective_rate(struct bridge *b, unsigned channels)
{
    uint64_t max = slogic_max_samplerate(&b->info, channels);
    if (b->rate <= max)
        return b->rate;
    for (int i = b->nrates - 1; i >= 0; i--)
        if (b->rates[i] <= max) {
            fprintf(stderr, "%g MHz too high for %u channels: %g MHz (disable channels for more).\n",
                b->rate/1e6, channels, b->rates[i]/1e6);
            return b->rates[i];
        }
    return max;
}

static int64_t capture_waveform(struct bridge *b, uint16_t *wfm, uint64_t rate)
{
    unsigned channels = capture_channels(b);
    double fs = FS_PER_SECOND/rate;
    uint64_t pre = b->trig_delay_fs > 0 ? (uint64_t)(b->trig_delay_fs/fs) : 0;
    struct slogic_config cfg = { .samplerate = rate, .channels = channels };
    uint16_t *ring = NULL, *tmp = NULL;
    uint64_t ring_n = 0, ring_pos = 0, post = 0, total = 0;
    int triggered = b->trig_src < 0 || b->force, prev = -1;
    const uint8_t *data;
    size_t size, tmp_size = 0;
    int r;

    if (pre >= b->depth)
        pre = b->depth - 1;
    if (triggered) {
        pre = 0;
        cfg.limit = b->depth; /* Free running/forced: exactly depth samples. */
    }
    if (slogic_config_check(&b->info, &cfg, NULL, 0))
        return -1;
    ring = calloc(pre + 1, sizeof(*ring));
    if ((r = slogic_capture_start(b->dev, &cfg))) {
        free(ring);
        return -1;
    }
    for (;;) {
        scpi_poll(b);
        if (b->armed == ARM_STOPPED || quit)
            break;
        if (b->force && !triggered) {
            triggered = 1; /* Forced: the next samples. */
            pre = 0;
        }
        r = slogic_capture_read(b->dev, &data, &size, 20);
        if (r == SLOGIC_ERR_TIMEOUT)
            continue;
        if (r)
            break;
        uint64_t n = (uint64_t)size*8/channels;
        if (n*2 > tmp_size) {
            tmp_size = n*2;
            tmp = realloc(tmp, tmp_size*sizeof(*tmp));
        }
        /* Unpacked to 16-bit values. */
        uint8_t *u = (uint8_t *)tmp + n*2; /* Second half of tmp as scratch. */
        slogic_unpack(channels, data, total, n, u);
        for (uint64_t i = 0; i < n; i++)
            tmp[i] = slogic_unitsize(channels) == 2 ? (uint16_t)(u[2*i] | u[2*i + 1] << 8) : u[i];
        total += n;
        for (uint64_t i = 0; i < n && post < b->depth - pre; i++) {
            uint16_t v = tmp[i];
            if (!triggered) {
                int cur = (v >> b->trig_src) & 1;
                /* Armed once the pre-trigger samples are available. */
                if (prev >= 0 && ring_n >= pre && edge_match(b->trig_edge, prev, cur)) {
                    triggered = 1;
                    /* Pre-trigger samples from the ring (oldest first). */
                    uint64_t have = ring_n < pre ? ring_n : pre;
                    for (uint64_t k = 0; k < have; k++)
                        wfm[k] = ring[(ring_pos + (pre + 1) - have + k) % (pre + 1)];
                    if (have < pre) { /* Not enough history: shift. */
                        pre = have;
                    }
                } else {
                    prev = cur;
                    ring[ring_pos] = v;
                    ring_pos = (ring_pos + 1) % (pre + 1);
                    ring_n++;
                    continue;
                }
            }
            wfm[pre + post++] = v;
        }
        if (triggered && post >= b->depth - pre)
            break;
    }
    slogic_capture_stop(b->dev, NULL);
    free(ring);
    free(tmp);
    if (!triggered || post < b->depth - pre)
        return -1;
    return (int64_t)pre;
}

static int send_waveform(struct bridge *b)
{
    uint16_t *wfm = calloc(b->depth, sizeof(*wfm));
    uint64_t bytes = (b->depth + 7)/8;
    uint8_t *bits = malloc(bytes);
    int64_t pre;
    int r = -1;

    if (!wfm || !bits)
        goto out;
    uint64_t rate = effective_rate(b, capture_channels(b));
    pre = capture_waveform(b, wfm, rate);
    if (pre < 0)
        goto out;
    b->force = 0;
    if (b->armed == ARM_SINGLE)
        b->armed = ARM_STOPPED;

    double now = slogic_time();
    if (b->last_wfm_t > 0)
        b->wfms = 1.0/(now - b->last_wfm_t);
    b->last_wfm_t = now;

    uint16_t nch = 0;
    for (int i = 0; i < 16; i++)
        nch += (b->enabled >> i) & 1;
    int64_t fs   = (int64_t)(FS_PER_SECOND/rate);
    int64_t trig = pre*fs;
    uint32_t seq = b->seqnum++;
    if (send_all(b->data_fd, &seq, 4) || send_all(b->data_fd, &nch, 2) || send_all(b->data_fd, &fs, 8) ||
        send_all(b->data_fd, &trig, 8) || send_all(b->data_fd, &b->wfms, 8))
        goto out;
    for (uint64_t ch = 0; ch < 16; ch++) {
        if (!((b->enabled >> ch) & 1))
            continue;
        uint64_t chnum = ch;
        int32_t first  = 0;
        memset(bits, 0, bytes);
        for (uint64_t i = 0; i < b->depth; i++)
            bits[i/8] |= ((wfm[i] >> ch) & 1) << (i % 8);
        if (send_all(b->data_fd, &chnum, 8) || send_all(b->data_fd, &bytes, 8) ||
            send_all(b->data_fd, &first, 4) || send_all(b->data_fd, bits, bytes))
            goto out;
    }
    r = 0;
out:
    free(wfm);
    free(bits);
    return r;
}

/* Main */
/*------*/

static void help(void)
{
    printf("slogic16u3_scopehal_bridge " SLOGIC_VERSION_STRING ": SLogic16U3 to ngscopeclient bridge.\n"
           "usage: slogic16u3_scopehal_bridge [-d uri] [--scpi-port N] [--waveform-port N]\n");
}

int main(int argc, char **argv)
{
    static const struct option opts[] = {
        { "scpi-port",     required_argument, NULL, 's' },
        { "waveform-port", required_argument, NULL, 'w' },
        { "help",          no_argument,       NULL, 'h' },
        { NULL, 0, NULL, 0 },
    };
    struct bridge b = { .rate = 100000000, .depth = 100000, .trig_src = -1, .enabled = 0xffff,
                        .scpi_fd = -1, .data_fd = -1 };
    const char *uri = NULL;
    int scpi_port = 5025, wfm_port = 5026, c, r;
    int pending = 0;

    while ((c = getopt_long(argc, argv, "d:h", opts, NULL)) != -1) {
        switch (c) {
        case 'd': uri = optarg; break;
        case 's': scpi_port = atoi(optarg); break;
        case 'w': wfm_port = atoi(optarg); break;
        default:  help(); return 1;
        }
    }
    if ((r = slogic_open(uri, &b.dev))) {
        fprintf(stderr, "Could not open the device: %s\n", slogic_strerror(r));
        return 1;
    }
    slogic_get_info(b.dev, &b.info);
    b.nrates = slogic_samplerates(&b.info, 4, b.rates, 64);

    int scpi_listen = listen_on(scpi_port), wfm_listen = listen_on(wfm_port);
    if (scpi_listen < 0 || wfm_listen < 0) {
        fprintf(stderr, "Could not listen on ports %d/%d\n", scpi_port, wfm_port);
        return 1;
    }
    signal(SIGINT, on_signal);
    signal(SIGTERM, on_signal);
    fprintf(stderr, "Bridge ready: %s, SCPI port %d, waveform port %d\n", b.info.ident, scpi_port, wfm_port);

    while (!quit) {
        fd_set rd;
        int maxfd = scpi_listen > wfm_listen ? scpi_listen : wfm_listen;
        struct timeval tv = { 0, 50000 };

        FD_ZERO(&rd);
        FD_SET(scpi_listen, &rd);
        FD_SET(wfm_listen, &rd);
        if (b.scpi_fd >= 0) { FD_SET(b.scpi_fd, &rd); if (b.scpi_fd > maxfd) maxfd = b.scpi_fd; }
        if (b.data_fd >= 0) { FD_SET(b.data_fd, &rd); if (b.data_fd > maxfd) maxfd = b.data_fd; }
        if (select(maxfd + 1, &rd, NULL, NULL, &tv) < 0)
            continue;
        if (FD_ISSET(scpi_listen, &rd)) {
            int fd = accept(scpi_listen, NULL, NULL), one = 1;
            if (b.scpi_fd >= 0)
                close(b.scpi_fd);
            setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
            b.scpi_fd  = fd;
            b.line_len = 0;
        }
        if (FD_ISSET(wfm_listen, &rd)) {
            int fd = accept(wfm_listen, NULL, NULL);
            if (b.data_fd >= 0)
                close(b.data_fd);
            b.data_fd = fd;
            pending   = 0;
        }
        if (b.scpi_fd >= 0 && FD_ISSET(b.scpi_fd, &rd))
            scpi_poll(&b);
        if (b.data_fd >= 0 && FD_ISSET(b.data_fd, &rd)) {
            uint8_t k;
            if (recv(b.data_fd, &k, 1, 0) <= 0) {
                close(b.data_fd);
                b.data_fd = -1;
                pending = 0;
            } else if (k == 'K') {
                pending++;
            }
        }
        while (pending && b.armed != ARM_STOPPED && b.data_fd >= 0 && !quit) {
            if (send_waveform(&b))
                break;
            pending--;
        }
    }
    if (b.scpi_fd >= 0) close(b.scpi_fd);
    if (b.data_fd >= 0) close(b.data_fd);
    close(scpi_listen);
    close(wfm_listen);
    slogic_close(b.dev);
    return 0;
}
