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

/*
 * Sipeed SLogic16U3 with the open LiteX gateware (USB3 in the FPGA transceivers, LiteX CommUSB
 * register accesses over EP0, capture data over Bulk IN), through libslogic:
 *
 *   sigrok-cli -d litex-slogic --samples 1M -c samplerate=100M
 *   sigrok-cli -d litex-slogic:conn=sim ...        (simulated device, no hardware)
 */

#include <config.h>
#include <string.h>
#include "protocol.h"

static const uint32_t scanopts[] = {
	SR_CONF_CONN,
};

static const uint32_t drvopts[] = {
	SR_CONF_LOGIC_ANALYZER,
};

static const uint32_t devopts[] = {
	SR_CONF_CONTINUOUS,
	SR_CONF_CONN | SR_CONF_GET,
	SR_CONF_SAMPLERATE | SR_CONF_GET | SR_CONF_SET | SR_CONF_LIST,
	SR_CONF_LIMIT_SAMPLES | SR_CONF_GET | SR_CONF_SET,
	SR_CONF_LIMIT_MSEC | SR_CONF_GET | SR_CONF_SET,
	SR_CONF_TRIGGER_MATCH | SR_CONF_LIST,
	SR_CONF_CAPTURE_RATIO | SR_CONF_GET | SR_CONF_SET,
	SR_CONF_PATTERN_MODE | SR_CONF_GET | SR_CONF_SET | SR_CONF_LIST,
};

static const int32_t trigger_matches[] = {
	SR_TRIGGER_ZERO,
	SR_TRIGGER_ONE,
	SR_TRIGGER_RISING,
	SR_TRIGGER_FALLING,
	SR_TRIGGER_EDGE,
};

static const char *patterns[] = {
	"inputs",
	"counter",
};

static GSList *scan(struct sr_dev_driver *di, GSList *options)
{
	struct sr_dev_inst *sdi;
	struct dev_context *devc;
	struct slogic_dev *dev;
	const char *conn = "usb";
	char name[16];

	for (GSList *l = options; l; l = l->next) {
		struct sr_config *src = l->data;
		if (src->key == SR_CONF_CONN)
			conn = g_variant_get_string(src->data, NULL);
	}
	if (slogic_open(conn, &dev) != SLOGIC_OK)
		return NULL;

	devc = g_malloc0(sizeof(*devc));
	devc->conn = g_strdup(conn);
	slogic_get_info(dev, &devc->info);
	slogic_close(dev);

	/* Samplerates: all the 16 channels ones, plus the higher 8/4 channels ones. */
	devc->num_samplerates = slogic_samplerates(&devc->info, 4, devc->samplerates, LITEX_SLOGIC_MAX_RATES);
	devc->cur_samplerate  = SR_MHZ(100);
	devc->capture_ratio   = 10;

	sdi = g_malloc0(sizeof(*sdi));
	sdi->status  = SR_ST_INACTIVE;
	sdi->vendor  = g_strdup("Sipeed");
	sdi->model   = g_strdup("SLogic16U3 (LiteX)");
	sdi->version = g_strdup_printf("%u.%u", devc->info.version >> 16, devc->info.version & 0xffff);
	sdi->serial_num = g_strdup(devc->info.serial);
	sdi->connection_id = g_strdup(conn);
	sdi->priv = devc;
	for (unsigned i = 0; i < devc->info.nchannels; i++) {
		snprintf(name, sizeof(name), "CH%u", i);
		sr_channel_new(sdi, i, SR_CHANNEL_LOGIC, TRUE, name);
	}
	return std_scan_complete(di, g_slist_append(NULL, sdi));
}

static void clear_helper(struct dev_context *devc)
{
	g_free(devc->conn);
	g_free(devc->unpacked);
}

static int dev_clear(const struct sr_dev_driver *di)
{
	return std_dev_clear_with_callback(di, (std_dev_clear_callback)clear_helper);
}

static int dev_open(struct sr_dev_inst *sdi)
{
	struct dev_context *devc = sdi->priv;
	int r = slogic_open(devc->conn, &devc->dev);

	if (r != SLOGIC_OK) {
		sr_err("Could not open %s: %s.", devc->conn, slogic_strerror(r));
		return SR_ERR;
	}
	return SR_OK;
}

static int dev_close(struct sr_dev_inst *sdi)
{
	struct dev_context *devc = sdi->priv;

	slogic_close(devc->dev);
	devc->dev = NULL;
	return SR_OK;
}

static int config_get(uint32_t key, GVariant **data,
	const struct sr_dev_inst *sdi, const struct sr_channel_group *cg)
{
	struct dev_context *devc;

	(void)cg;
	if (!sdi)
		return SR_ERR_ARG;
	devc = sdi->priv;
	switch (key) {
	case SR_CONF_CONN:
		*data = g_variant_new_string(devc->conn);
		break;
	case SR_CONF_SAMPLERATE:
		*data = g_variant_new_uint64(devc->cur_samplerate);
		break;
	case SR_CONF_LIMIT_SAMPLES:
		*data = g_variant_new_uint64(devc->limit_samples);
		break;
	case SR_CONF_LIMIT_MSEC:
		*data = g_variant_new_uint64(devc->limit_msec);
		break;
	case SR_CONF_CAPTURE_RATIO:
		*data = g_variant_new_uint64(devc->capture_ratio);
		break;
	case SR_CONF_PATTERN_MODE:
		*data = g_variant_new_string(patterns[devc->pattern]);
		break;
	default:
		return SR_ERR_NA;
	}
	return SR_OK;
}

static int config_set(uint32_t key, GVariant *data,
	const struct sr_dev_inst *sdi, const struct sr_channel_group *cg)
{
	struct dev_context *devc = sdi->priv;
	int idx;

	(void)cg;
	switch (key) {
	case SR_CONF_SAMPLERATE: {
		uint64_t rate = g_variant_get_uint64(data);
		if (!rate || devc->info.base_rate % rate)
			return SR_ERR_ARG;
		devc->cur_samplerate = rate;
		break;
	}
	case SR_CONF_LIMIT_SAMPLES:
		devc->limit_samples = g_variant_get_uint64(data);
		break;
	case SR_CONF_LIMIT_MSEC:
		devc->limit_msec = g_variant_get_uint64(data);
		break;
	case SR_CONF_CAPTURE_RATIO:
		devc->capture_ratio = g_variant_get_uint64(data);
		break;
	case SR_CONF_PATTERN_MODE:
		if ((idx = std_str_idx(data, ARRAY_AND_SIZE(patterns))) < 0)
			return SR_ERR_ARG;
		devc->pattern = idx;
		break;
	default:
		return SR_ERR_NA;
	}
	return SR_OK;
}

static int config_list(uint32_t key, GVariant **data,
	const struct sr_dev_inst *sdi, const struct sr_channel_group *cg)
{
	struct dev_context *devc = sdi ? sdi->priv : NULL;

	switch (key) {
	case SR_CONF_SCAN_OPTIONS:
	case SR_CONF_DEVICE_OPTIONS:
		return STD_CONFIG_LIST(key, data, sdi, cg, scanopts, drvopts, devopts);
	case SR_CONF_SAMPLERATE:
		if (!devc)
			return SR_ERR_ARG;
		*data = std_gvar_samplerates(devc->samplerates, devc->num_samplerates);
		break;
	case SR_CONF_TRIGGER_MATCH:
		*data = std_gvar_array_i32(ARRAY_AND_SIZE(trigger_matches));
		break;
	case SR_CONF_PATTERN_MODE:
		*data = g_variant_new_strv(ARRAY_AND_SIZE(patterns));
		break;
	default:
		return SR_ERR_NA;
	}
	return SR_OK;
}

static int dev_acquisition_start(const struct sr_dev_inst *sdi)
{
	struct dev_context *devc = sdi->priv;
	struct sr_trigger *trigger;
	struct slogic_config cfg;
	char msg[160];
	int r;

	devc->channels = litex_slogic_channels_mode(sdi);
	devc->unitsize = slogic_unitsize(devc->channels);
	/* Higher channel modes when the samplerate needs it are not possible: report the limit. */
	cfg.samplerate = devc->cur_samplerate;
	cfg.channels   = devc->channels;
	cfg.pattern    = devc->pattern;
	cfg.limit      = 0;
	devc->received_samples = 0;
	devc->sent_samples     = 0;
	devc->acq_done         = FALSE;
	devc->trigger_fired    = TRUE;
	devc->stl              = NULL;

	if ((trigger = sr_session_trigger_get(sdi->session))) {
		int pre = devc->limit_samples ? (int)(devc->capture_ratio*devc->limit_samples/100) : 0;
		devc->stl = soft_trigger_logic_new(sdi, trigger, pre);
		if (!devc->stl)
			return SR_ERR_MALLOC;
		devc->trigger_fired = FALSE;
	} else {
		/* No trigger: exact sample count captured by the gateware. */
		cfg.limit = devc->limit_samples;
	}
	if (slogic_config_check(&devc->info, &cfg, msg, sizeof(msg)) != SLOGIC_OK) {
		sr_err("Invalid configuration: %s (enable fewer channels: 4/8/16 channels modes).", msg);
		return SR_ERR_ARG;
	}
	if ((r = slogic_capture_start(devc->dev, &cfg)) != SLOGIC_OK) {
		sr_err("Capture start: %s.", slogic_strerror(r));
		return SR_ERR;
	}
	devc->start_us = g_get_monotonic_time();
	std_session_send_df_header(sdi);
	sr_session_source_add(sdi->session, -1, 0, 1, litex_slogic_receive_data, (void *)sdi);
	devc->acq_running = TRUE;
	return SR_OK;
}

static int dev_acquisition_stop(struct sr_dev_inst *sdi)
{
	struct dev_context *devc = sdi->priv;
	struct slogic_status st;

	if (!devc->acq_running)
		return SR_OK; /* Start failed. */
	devc->acq_running = FALSE;
	sr_session_source_remove(sdi->session, -1);
	slogic_capture_stop(devc->dev, &st);
	if (st.overflow)
		sr_warn("Capture stopped on overflow after %" PRIu64 " samples.", st.samples);
	std_session_send_df_end(sdi);
	if (devc->stl) {
		soft_trigger_logic_free(devc->stl);
		devc->stl = NULL;
	}
	return SR_OK;
}

static struct sr_dev_driver litex_slogic_driver_info = {
	.name = "litex-slogic",
	.longname = "Sipeed SLogic16U3 (LiteX gateware)",
	.api_version = 1,
	.init = std_init,
	.cleanup = std_cleanup,
	.scan = scan,
	.dev_list = std_dev_list,
	.dev_clear = dev_clear,
	.config_get = config_get,
	.config_set = config_set,
	.config_list = config_list,
	.dev_open = dev_open,
	.dev_close = dev_close,
	.dev_acquisition_start = dev_acquisition_start,
	.dev_acquisition_stop = dev_acquisition_stop,
	.context = NULL,
};
SR_REGISTER_DEV_DRIVER(litex_slogic_driver_info);
