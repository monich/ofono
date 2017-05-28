/*
 *  oFono - Open Source Telephony - RIL-based devices
 *
 *  Copyright (C) 2015-2017 Jolla Ltd.
 *
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License version 2 as
 *  published by the Free Software Foundation.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 */

#include "ril_plugin.h"
#include "ril_sim_settings.h"
#include "ril_network.h"
#include "ril_util.h"
#include "ril_log.h"

#include <gutil_idlequeue.h>
#include <gutil_ints.h>

enum ril_radio_settings_cb_tag {
	RADIO_SETTINGS_QUERY_AVAILABLE_RATS = 1,
	RADIO_SETTINGS_QUERY_AVAILABLE_MODES,
	RADIO_SETTINGS_QUERY_RAT_MODE,
	RADIO_SETTINGS_SET_RAT_MODE
};

struct ril_radio_settings {
	GUtilIdleQueue *iq;
	GUtilInts *supported_modes;
	GHashTable *legacy_rat_map;
	struct ofono_radio_settings *rs;
	struct ril_sim_settings *settings;
	const char *log_prefix;
	char *allocated_log_prefix;
};

struct ril_radio_settings_cbd {
	struct ril_radio_settings *rsd;
	union _ofono_radio_settings_cb {
		ofono_radio_settings_rat_mode_set_cb_t rat_mode_set;
		ofono_radio_settings_rat_mode_query_cb_t rat_mode_query;
		ofono_radio_settings_available_rats_query_cb_t available_rats;
		ofono_radio_settings_available_modes_query_cb_t available_modes;
		gpointer ptr;
	} cb;
	gpointer data;
};

#define DBG_(rsd,fmt,args...) DBG("%s" fmt, (rsd)->log_prefix, ##args)

static inline struct ril_radio_settings *ril_radio_settings_get_data(
					struct ofono_radio_settings *rs)
{
	return ofono_radio_settings_get_data(rs);
}

static void ril_radio_settings_cbd_free(gpointer data)
{
	g_slice_free(struct ril_radio_settings_cbd, data);
}

static void ril_radio_settings_later(struct ril_radio_settings *rsd,
			enum ril_radio_settings_cb_tag tag, GUtilIdleFunc fn,
			void *cb, void *data)
{
	struct ril_radio_settings_cbd *cbd;

	cbd = g_slice_new0(struct ril_radio_settings_cbd);
	cbd->rsd = rsd;
	cbd->cb.ptr = cb;
	cbd->data = data;

	GVERIFY_FALSE(gutil_idle_queue_cancel_tag(rsd->iq, tag));
	gutil_idle_queue_add_tag_full(rsd->iq, tag, fn, cbd,
					ril_radio_settings_cbd_free);
}

static void ril_radio_settings_set_rat_mode_cb(gpointer user_data)
{
	struct ril_radio_settings_cbd *cbd = user_data;
	struct ofono_error error;

	cbd->cb.rat_mode_set(ril_error_ok(&error), cbd->data);
}

static void ril_radio_settings_set_rat_mode_error_cb(gpointer user_data)
{
	struct ril_radio_settings_cbd *cbd = user_data;
	struct ofono_error error;

	cbd->cb.rat_mode_set(ril_error_failure(&error), cbd->data);
}

static void ril_radio_settings_set_rat_mode(struct ofono_radio_settings *rs,
		enum ofono_radio_access_mode mode,
		ofono_radio_settings_rat_mode_set_cb_t cb, void *data)
{
	struct ril_radio_settings *rsd = ril_radio_settings_get_data(rs);

	DBG_(rsd, "%s", ril_access_mode_to_string(mode));
	if (mode == OFONO_RADIO_ACCESS_MODE_ANY ||
			gutil_ints_contains(rsd->supported_modes, mode)) {
		ril_sim_settings_set_pref_mode(rsd->settings, mode);
		ril_radio_settings_later(rsd, RADIO_SETTINGS_SET_RAT_MODE,
			ril_radio_settings_set_rat_mode_cb, cb, data);
	} else {
		/* Refuse to accept unsupported modes */
		ril_radio_settings_later(rsd, RADIO_SETTINGS_SET_RAT_MODE,
			ril_radio_settings_set_rat_mode_error_cb, cb, data);
	}
}

static void ril_radio_settings_query_rat_mode_cb(gpointer user_data)
{
	struct ril_radio_settings_cbd *cbd = user_data;
	struct ril_radio_settings *rsd = cbd->rsd;
	enum ofono_radio_access_mode mode = rsd->settings->pref_mode;
	struct ofono_error error;

	DBG_(rsd, "rat mode %s", ril_access_mode_to_string(mode));
	cbd->cb.rat_mode_query(ril_error_ok(&error), mode, cbd->data);
}

static void ril_radio_settings_query_rat_mode(struct ofono_radio_settings *rs,
		ofono_radio_settings_rat_mode_query_cb_t cb, void *data)
{
	struct ril_radio_settings *rsd = ril_radio_settings_get_data(rs);

	DBG_(rsd, "");
	ril_radio_settings_later(rsd, RADIO_SETTINGS_QUERY_RAT_MODE,
			ril_radio_settings_query_rat_mode_cb, cb, data);
}

static void ril_radio_settings_query_available_rats_cb(gpointer data)
{
	struct ofono_error error;
	struct ril_radio_settings_cbd *cbd = data;
	struct ril_radio_settings *rsd = cbd->rsd;

	cbd->cb.available_rats(ril_error_ok(&error), rsd->settings->techs,
								cbd->data);
}

static void ril_radio_settings_query_available_rats(
		struct ofono_radio_settings *rs,
		ofono_radio_settings_available_rats_query_cb_t cb, void *data)
{
	struct ril_radio_settings *rsd = ril_radio_settings_get_data(rs);

	DBG_(rsd, "");
	ril_radio_settings_later(rsd, RADIO_SETTINGS_QUERY_AVAILABLE_RATS,
			ril_radio_settings_query_available_rats_cb, cb, data);
}

static void ril_radio_settings_query_available_modes_cb(gpointer data)
{
	guint i, n;
	struct ofono_error error;
	struct ril_radio_settings_cbd *cbd = data;
	struct ril_radio_settings *rsd = cbd->rsd;
	const int* value = gutil_ints_get_data(rsd->supported_modes, &n);
	enum ofono_radio_access_mode *modes;

	/* Copy those, enum doesn't have to have to size of an int */
	modes = g_new(enum ofono_radio_access_mode, n + 1);
	for (i = 0; i < n; i++) {
		modes[i] = value[i];
	}
	modes[i] = 0;

	cbd->cb.available_modes(ril_error_ok(&error), modes, cbd->data);
	g_free(modes);
}

static void ril_radio_settings_query_available_modes(
		struct ofono_radio_settings *rs,
		ofono_radio_settings_available_modes_query_cb_t cb, void *data)
{
	struct ril_radio_settings *rsd = ril_radio_settings_get_data(rs);

	DBG_(rsd, "");
	ril_radio_settings_later(rsd, RADIO_SETTINGS_QUERY_AVAILABLE_MODES,
			ril_radio_settings_query_available_modes_cb, cb, data);
}

static enum ofono_radio_access_mode ril_radio_settings_map_legacy_rat_mode
	(struct ofono_radio_settings *rs, enum ofono_radio_access_mode rat)
{
	struct ril_radio_settings *rsd = ril_radio_settings_get_data(rs);
	gpointer mapped = g_hash_table_lookup(rsd->legacy_rat_map,
							GINT_TO_POINTER(rat));
	return (enum ofono_radio_access_mode)GPOINTER_TO_INT(mapped);
}

static void ril_radio_settings_register(gpointer user_data)
{
	struct ril_radio_settings *rsd = user_data;

	DBG_(rsd, "");
	ofono_radio_settings_register(rsd->rs);
}

static int ril_radio_settings_probe(struct ofono_radio_settings *rs,
					unsigned int vendor, void *data)
{
	struct ril_modem *modem = data;
	struct ril_radio_settings *rsd = g_new0(struct ril_radio_settings, 1);
	guint r, n;
	GUtilInts *modes = ril_network_supported_modes(modem->network);
	const int* val = gutil_ints_get_data(modes, &n);

	DBG("%s", modem->log_prefix);
	rsd->rs = rs;
	rsd->settings = ril_sim_settings_ref(modem->sim_settings);
	rsd->supported_modes = gutil_ints_ref(modes);
	rsd->iq = gutil_idle_queue_new();
	gutil_idle_queue_add(rsd->iq, ril_radio_settings_register, rsd);

	if (modem->log_prefix && modem->log_prefix[0]) {
		rsd->log_prefix = rsd->allocated_log_prefix =
			g_strconcat(modem->log_prefix, " ", NULL);
	} else {
		rsd->log_prefix = "";
	}

	/* Fill the legacy access mode map */
	rsd->legacy_rat_map = g_hash_table_new(g_direct_hash, g_direct_equal);
	for (r = 1; r & OFONO_RADIO_ACCESS_MODE_ALL; r <<= 1) {
		guint i, max = 0;
		/* These bits have to be off: */
		const int off = ~((r << 1) - 1);

		/* Find the largest (e.g. most functional) mode */
		for (i = 0; i < n; i++) {
			const int m = val[i];

			if (!(m & off) && m > max) {
				max = m;
			}
		}
		DBG_(rsd, "%s -> 0x%x", ril_access_mode_to_string(r), max);
		if (max) {
			g_hash_table_insert(rsd->legacy_rat_map,
				GINT_TO_POINTER(r), GINT_TO_POINTER(max));
		}
	}

	ofono_radio_settings_set_data(rs, rsd);
	return 0;
}

static void ril_radio_settings_remove(struct ofono_radio_settings *rs)
{
	struct ril_radio_settings *rsd = ril_radio_settings_get_data(rs);

	DBG_(rsd, "");
	g_hash_table_destroy(rsd->legacy_rat_map);
	ofono_radio_settings_set_data(rs, NULL);
	gutil_ints_unref(rsd->supported_modes);
	gutil_idle_queue_cancel_all(rsd->iq);
	gutil_idle_queue_unref(rsd->iq);
	ril_sim_settings_unref(rsd->settings);
	g_free(rsd->allocated_log_prefix);
	g_free(rsd);
}

const struct ofono_radio_settings_driver ril_radio_settings_driver = {
	.name                      = RILMODEM_DRIVER,
	.probe                     = ril_radio_settings_probe,
	.remove                    = ril_radio_settings_remove,
	.query_rat_mode            = ril_radio_settings_query_rat_mode,
	.set_rat_mode              = ril_radio_settings_set_rat_mode,
	.query_available_rats      = ril_radio_settings_query_available_rats,
	.query_available_rat_modes = ril_radio_settings_query_available_modes,
	.map_legacy_rat_mode       = ril_radio_settings_map_legacy_rat_mode
};

/*
 * Local Variables:
 * mode: C
 * c-basic-offset: 8
 * indent-tabs-mode: t
 * End:
 */
