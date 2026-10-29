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

#include <config.h>
#include <string.h>
#include "protocol.h"

/*
 * Capture mode: the smallest gateware channel mode (4/8/16) holding the highest enabled channel
 * (the gateware captures CH0..CHn-1).
 */
SR_PRIV unsigned litex_slogic_channels_mode(const struct sr_dev_inst *sdi)
{
	int highest = -1;

	for (GSList *l = sdi->channels; l; l = l->next) {
		struct sr_channel *ch = l->data;
		if (ch->enabled && ch->index > highest)
			highest = ch->index;
	}
	if (highest < 4)
		return 4;
	if (highest < 8)
		return 8;
	return 16;
}

static void send_logic(const struct sr_dev_inst *sdi, const uint8_t *data, uint64_t samples)
{
	struct dev_context *devc = sdi->priv;
	struct sr_datafeed_logic logic = {
		.length   = samples*devc->unitsize,
		.unitsize = devc->unitsize,
		.data     = (void *)data,
	};
	struct sr_datafeed_packet packet = {
		.type    = SR_DF_LOGIC,
		.payload = &logic,
	};

	if (!samples)
		return;
	sr_session_send(sdi, &packet);
}

/* Samples to the session: soft trigger (pre-trigger samples sent by the trigger code), sample limit. */
static void process_samples(const struct sr_dev_inst *sdi, uint8_t *data, uint64_t samples)
{
	struct dev_context *devc = sdi->priv;
	uint64_t offset = 0;

	if (!devc->trigger_fired) {
		int pre_trigger_samples = 0;
		int trigger_offset = soft_trigger_logic_check(devc->stl, data, samples*devc->unitsize,
			&pre_trigger_samples);
		if (trigger_offset < 0)
			return;
		devc->sent_samples += pre_trigger_samples;
		devc->trigger_fired = TRUE;
		offset = trigger_offset;
	}
	uint64_t n = samples - offset;
	if (devc->limit_samples && devc->sent_samples + n > devc->limit_samples)
		n = devc->limit_samples - devc->sent_samples;
	send_logic(sdi, data + offset*devc->unitsize, n);
	devc->sent_samples += n;
	if (devc->limit_samples && devc->sent_samples >= devc->limit_samples)
		devc->acq_done = TRUE;
}

SR_PRIV int litex_slogic_receive_data(int fd, int revents, void *cb_data)
{
	const struct sr_dev_inst *sdi = cb_data;
	struct dev_context *devc;
	const uint8_t *data;
	size_t size;
	int r = SLOGIC_OK;

	(void)fd;
	(void)revents;

	if (!sdi || !(devc = sdi->priv))
		return TRUE;

	/* Drain the received buffers (bounded: the session loop keeps running). */
	for (int i = 0; i < 64 && !devc->acq_done; i++) {
		r = slogic_capture_read(devc->dev, &data, &size, i ? 0 : LITEX_SLOGIC_READ_MS);
		if (r != SLOGIC_OK)
			break;
		uint64_t samples = (uint64_t)size*8/devc->channels;
		if (samples*devc->unitsize > devc->unpacked_size) {
			devc->unpacked_size = samples*devc->unitsize;
			devc->unpacked = g_realloc(devc->unpacked, devc->unpacked_size);
		}
		slogic_unpack(devc->channels, data, devc->received_samples, samples, devc->unpacked);
		devc->received_samples += samples;
		process_samples(sdi, devc->unpacked, samples);
	}

	if (r == SLOGIC_ERR_OVERFLOW) {
		sr_err("Capture overflow: data lost (USB bandwidth), lower the samplerate or channels.");
		devc->acq_done = TRUE;
	} else if (r == SLOGIC_ERR_END) {
		devc->acq_done = TRUE;
	} else if (r != SLOGIC_OK && r != SLOGIC_ERR_TIMEOUT) {
		sr_err("Capture error: %s.", slogic_strerror(r));
		devc->acq_done = TRUE;
	}
	if (devc->limit_msec && (uint64_t)(g_get_monotonic_time() - devc->start_us) >= devc->limit_msec*1000)
		devc->acq_done = TRUE;

	if (devc->acq_done)
		sr_dev_acquisition_stop((struct sr_dev_inst *)sdi);
	return TRUE;
}
