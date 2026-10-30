/*
 * This file is part of the libsigrok project.
 *
 * Copyright (C) 2026 Enjoy-Digital <enjoy-digital.fr>
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#ifndef LIBSIGROK_HARDWARE_LITEX_SLOGIC_PROTOCOL_H
#define LIBSIGROK_HARDWARE_LITEX_SLOGIC_PROTOCOL_H

#include <glib.h>
#include <stdint.h>
#include <libsigrok/libsigrok.h>
#include "libsigrok-internal.h"

/* libslogic (BSD-2-Clause): SLogic16U3 LiteX gateware host library. */
#include "libslogic/slogic.h"

#define LOG_PREFIX "litex-slogic"

#define LITEX_SLOGIC_MAX_RATES  64
#define LITEX_SLOGIC_READ_MS    10

struct dev_context {
	char *conn;                    /* libslogic URI ("usb", "usb:serial=...", "sim"). */
	struct slogic_dev *dev;
	struct slogic_info info;

	uint64_t samplerates[LITEX_SLOGIC_MAX_RATES];
	int num_samplerates;

	uint64_t cur_samplerate;
	uint64_t limit_samples;
	uint64_t limit_msec;
	uint64_t capture_ratio;
	int pattern;                   /* 0: inputs, 1: counter test pattern, 2: pattern generator. */

	/* Acquisition. */
	unsigned channels;             /* Capture mode: 4, 8 or 16 channels. */
	unsigned unitsize;
	uint64_t received_samples;     /* Samples received from the device. */
	uint64_t sent_samples;         /* Samples sent to the session (after the trigger). */
	int64_t start_us;
	uint8_t *unpacked;
	size_t unpacked_size;
	struct soft_trigger_logic *stl;
	gboolean trigger_fired;
	gboolean acq_done;
	gboolean acq_running;           /* Session source added. */
};

SR_PRIV int litex_slogic_receive_data(int fd, int revents, void *cb_data);
SR_PRIV unsigned litex_slogic_channels_mode(const struct sr_dev_inst *sdi);

#endif
