/*
 *
 *  oFono - Open Source Telephony
 *
 *  Copyright (C) 2010  Nokia Corporation and/or its subsidiary(-ies).
 *
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License version 2 as
 *  published by the Free Software Foundation.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with this program; if not, write to the Free Software
 *  Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA  02110-1301  USA
 *
 */

#ifdef HAVE_CONFIG_H
#include <config.h>
#endif

#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <errno.h>
#include <stdint.h>

#include <glib.h>
#include <gdbus.h>

#include "ofono.h"
#include "common.h"
#include "storage.h"
#include "dbus-queue.h"

#define SETTINGS_STORE "radiosetting"
#define SETTINGS_GROUP "Settings"
#define RADIO_SETTINGS_FLAG_CACHED 0x1

static GSList *g_drivers = NULL;

struct ofono_radio_settings {
	struct ofono_dbus_queue *q;
	int flags;
	int mode; /* rat mask or -legacy mode */
	enum ofono_radio_band_gsm band_gsm;
	enum ofono_radio_band_umts band_umts;
	ofono_bool_t fast_dormancy;
	int pending_mode;
	enum ofono_radio_band_gsm pending_band_gsm;
	enum ofono_radio_band_umts pending_band_umts;
	ofono_bool_t fast_dormancy_pending;
	enum ofono_radio_access_mode *available_modes;
	GKeyFile *settings;
	char *imsi;
	const struct ofono_radio_settings_driver *driver;
	void *driver_data;
	struct ofono_atom *atom;
};

static const enum ofono_radio_access_mode legacy_modes[] = {
	OFONO_RADIO_ACCESS_MODE_GSM,
	OFONO_RADIO_ACCESS_MODE_UMTS,
	OFONO_RADIO_ACCESS_MODE_LTE
};

static const char *radio_legacy_mode_to_string(enum ofono_radio_access_mode m)
{
	switch (m) {
	case OFONO_RADIO_ACCESS_MODE_ANY:
		return "any";
	case OFONO_RADIO_ACCESS_MODE_GSM:
		return "gsm";
	case OFONO_RADIO_ACCESS_MODE_UMTS:
		return "umts";
	case OFONO_RADIO_ACCESS_MODE_LTE:
		return "lte";
	default:
		return NULL;
	}
}

static const char *radio_access_modes_to_string(enum ofono_radio_access_mode m)
{
	static const char *mode_string[OFONO_RADIO_ACCESS_MODE_ALL+1] = {
		"any",
		"+gsm",
		"+umts",
		"+umts+gsm",
		"+lte",
		"+lte+gsm",
		"+lte+umts",
		"+lte+umts+gsm"
	};

	return mode_string[m & OFONO_RADIO_ACCESS_MODE_ALL];
}

static const char *radio_access_mode_to_string(int m)
{
	return m < 0 ? radio_legacy_mode_to_string(-m) :
		radio_access_modes_to_string(m);
}

static gboolean radio_access_mode_is_supported(struct ofono_radio_settings *rs,
					enum ofono_radio_access_mode mode)
{
	/* ANY is always supported */
	if (mode == OFONO_RADIO_ACCESS_MODE_ANY)
		return TRUE;

	if (rs->available_modes) {
		const enum ofono_radio_access_mode *m = rs->available_modes;

		while (*m) {
			if (*m++ == mode) {
				return TRUE;
			}
		}

		return FALSE;
	}

	/*
	 * We have no idea what's supported and what's not, let's assume
	 * that everything is!
	 */
	return TRUE;
}

static gboolean radio_legacy_rat_driver(struct ofono_radio_settings *rs)
{
	/*
	 * query_available_rat_modes is provided by the drivers that support
	 * ofono_radio_access_mode mask, i.e. a set of preferred technologies.
	 */
	return !rs->driver->query_available_rat_modes;
}

static enum ofono_radio_access_mode radio_map_legacy_rat
				(struct ofono_radio_settings *rs,
					enum ofono_radio_access_mode rat)
{
	if (rat == OFONO_RADIO_ACCESS_MODE_ANY || radio_legacy_rat_driver(rs))
		return rat;

	if (rs->driver->map_legacy_rat_mode)
		return rs->driver->map_legacy_rat_mode(rs, rat);

	/* rat is supposed to be a single bit */
	return (rat | (rat - 1)) & OFONO_RADIO_ACCESS_MODE_ALL;
}

static gboolean radio_legacy_mode_from_string(const char *str,
					enum ofono_radio_access_mode *mode)

{
	if (g_str_equal(str, "any")) {
		*mode = OFONO_RADIO_ACCESS_MODE_ANY;
		return TRUE;
	} else if (g_str_equal(str, "gsm")) {
		*mode = OFONO_RADIO_ACCESS_MODE_GSM;
		return TRUE;
	} else if (g_str_equal(str, "umts")) {
		*mode = OFONO_RADIO_ACCESS_MODE_UMTS;
		return TRUE;
	} else if (g_str_equal(str, "lte")) {
		*mode = OFONO_RADIO_ACCESS_MODE_LTE;
		return TRUE;
	}

	return FALSE;
}

static gboolean radio_access_modes_from_string(const char *str,
					enum ofono_radio_access_mode *mask)
{
	if (str && str[0] == '+') {
		gboolean ok = TRUE, any = FALSE;
		char **modes = g_strsplit (str + 1, "+", -1);
		int i;

		*mask = 0;

		for (i = 0; modes[i] && ok; i++) {
			enum ofono_radio_access_mode m;
			const char *s = modes[i];

			if (radio_legacy_mode_from_string(s, &m)) {
				if (m == OFONO_RADIO_ACCESS_MODE_ANY)
					any = TRUE;
				else
					*mask |= m;
			} else {
				ok = FALSE;
			}
		}

		if (any)
			*mask = OFONO_RADIO_ACCESS_MODE_ANY;

		g_strfreev(modes);
		return ok;
	}
	return FALSE;
}

static const char *radio_band_gsm_to_string(enum ofono_radio_band_gsm band)
{
	switch (band) {
	case OFONO_RADIO_BAND_GSM_ANY:
		return "any";
	case OFONO_RADIO_BAND_GSM_850:
		return "850";
	case OFONO_RADIO_BAND_GSM_900P:
		return "900P";
	case OFONO_RADIO_BAND_GSM_900E:
		return "900E";
	case OFONO_RADIO_BAND_GSM_1800:
		return "1800";
	case OFONO_RADIO_BAND_GSM_1900:
		return "1900";
	}

	return NULL;
}

static gboolean radio_band_gsm_from_string(const char *str,
					enum ofono_radio_band_gsm *band)
{
	if (g_str_equal(str, "any")) {
		*band = OFONO_RADIO_BAND_GSM_ANY;
		return TRUE;
	} else if (g_str_equal(str, "850")) {
		*band = OFONO_RADIO_BAND_GSM_850;
		return TRUE;
	} else if (g_str_equal(str, "900P")) {
		*band = OFONO_RADIO_BAND_GSM_900P;
		return TRUE;
	} else if (g_str_equal(str, "900E")) {
		*band = OFONO_RADIO_BAND_GSM_900E;
		return TRUE;
	} else if (g_str_equal(str, "1800")) {
		*band = OFONO_RADIO_BAND_GSM_1800;
		return TRUE;
	} else if (g_str_equal(str, "1900")) {
		*band = OFONO_RADIO_BAND_GSM_1900;
		return TRUE;
	}

	return FALSE;
}

static const char *radio_band_umts_to_string(enum ofono_radio_band_umts band)
{
	switch (band) {
	case OFONO_RADIO_BAND_UMTS_ANY:
		return "any";
	case OFONO_RADIO_BAND_UMTS_850:
		return "850";
	case OFONO_RADIO_BAND_UMTS_900:
		return "900";
	case OFONO_RADIO_BAND_UMTS_1700AWS:
		return "1700AWS";
	case OFONO_RADIO_BAND_UMTS_1900:
		return "1900";
	case OFONO_RADIO_BAND_UMTS_2100:
		return "2100";
	}

	return NULL;
}

static gboolean radio_band_umts_from_string(const char *str,
					enum ofono_radio_band_umts *band)
{
	if (g_str_equal(str, "any")) {
		*band = OFONO_RADIO_BAND_UMTS_ANY;
		return TRUE;
	} else if (g_str_equal(str, "850")) {
		*band = OFONO_RADIO_BAND_UMTS_850;
		return TRUE;
	} else if (g_str_equal(str, "900")) {
		*band = OFONO_RADIO_BAND_UMTS_900;
		return TRUE;
	} else if (g_str_equal(str, "1700AWS")) {
		*band = OFONO_RADIO_BAND_UMTS_1700AWS;
		return TRUE;
	} else if (g_str_equal(str, "1900")) {
		*band = OFONO_RADIO_BAND_UMTS_1900;
		return TRUE;
	} else if (g_str_equal(str, "2100")) {
		*band = OFONO_RADIO_BAND_UMTS_2100;
		return TRUE;
	}

	return FALSE;
}

static DBusMessage *radio_get_properties_reply(DBusMessage *msg,
						struct ofono_radio_settings *rs)
{
	DBusMessage *reply;
	DBusMessageIter iter;
	DBusMessageIter dict;

	const char *mode = radio_access_mode_to_string(rs->mode);

	reply = dbus_message_new_method_return(msg);
	if (reply == NULL)
		return NULL;

	dbus_message_iter_init_append(reply, &iter);

	dbus_message_iter_open_container(&iter, DBUS_TYPE_ARRAY,
					OFONO_PROPERTIES_ARRAY_SIGNATURE,
					&dict);

	ofono_dbus_dict_append(&dict, "TechnologyPreference",
					DBUS_TYPE_STRING, &mode);

	if (rs->driver->query_band) {
		const char *band = radio_band_gsm_to_string(rs->band_gsm);

		ofono_dbus_dict_append(&dict, "GsmBand",
					DBUS_TYPE_STRING, &band);

		band = radio_band_umts_to_string(rs->band_umts);

		ofono_dbus_dict_append(&dict, "UmtsBand",
					DBUS_TYPE_STRING, &band);
	}

	if (rs->driver->query_fast_dormancy) {
		dbus_bool_t value = rs->fast_dormancy;
		ofono_dbus_dict_append(&dict, "FastDormancy",
					DBUS_TYPE_BOOLEAN, &value);
	}

	if (rs->available_modes) {
		int i = 0, n = 0;
		const char **dbus_rats;
		const enum ofono_radio_access_mode *m;

		while (rs->available_modes[n])
			n++;

		if (radio_legacy_rat_driver(rs)) {
			dbus_rats = g_new(const char *, n + 1);
			for (m = rs->available_modes; *m; m++) {
				dbus_rats[i++] =
					radio_legacy_mode_to_string(*m);
			}
		} else {
			int k;

			/* Add valid legacy modes */
			n += G_N_ELEMENTS(legacy_modes);
			dbus_rats = g_new(const char *, n + 1);

			for (k = 0; k < G_N_ELEMENTS(legacy_modes); k++) {
				enum ofono_radio_access_mode l =
					legacy_modes[k];

				if (radio_map_legacy_rat(rs, l)) {
					dbus_rats[i++] =
						radio_legacy_mode_to_string(l);
				}
			}

			/* and the combinations of modes */
			for (m = rs->available_modes; *m; m++) {
				dbus_rats[i++] =
					radio_access_modes_to_string(*m);
			}
		}

		dbus_rats[i] = NULL;
		ofono_dbus_dict_append_array(&dict, "AvailableTechnologies",
						DBUS_TYPE_STRING, &dbus_rats);
		g_free(dbus_rats);
	}

	dbus_message_iter_close_container(&iter, &dict);

	return reply;
}

static void radio_set_fast_dormancy(struct ofono_radio_settings *rs,
					ofono_bool_t enable)
{
	DBusConnection *conn = ofono_dbus_get_connection();
	const char *path = __ofono_atom_get_path(rs->atom);
	dbus_bool_t value = enable;

	if (rs->fast_dormancy == enable)
		return;

	ofono_dbus_signal_property_changed(conn, path,
						OFONO_RADIO_SETTINGS_INTERFACE,
						"FastDormancy",
						DBUS_TYPE_BOOLEAN, &value);
	rs->fast_dormancy = enable;
}

static void radio_fast_dormancy_set_callback(const struct ofono_error *error,
						void *data)
{
	struct ofono_radio_settings *rs = data;

	if (error->type != OFONO_ERROR_TYPE_NO_ERROR) {
		DBG("Error setting fast dormancy");

		rs->fast_dormancy_pending = rs->fast_dormancy;

		__ofono_dbus_queue_reply_failed(rs->q);

		return;
	}

	__ofono_dbus_queue_reply_ok(rs->q);

	radio_set_fast_dormancy(rs, rs->fast_dormancy_pending);
}

static void radio_set_band(struct ofono_radio_settings *rs)
{
	DBusConnection *conn = ofono_dbus_get_connection();
	const char *path;
	const char *str_band;

	path = __ofono_atom_get_path(rs->atom);

	if (rs->band_gsm != rs->pending_band_gsm) {
		rs->band_gsm = rs->pending_band_gsm;
		str_band = radio_band_gsm_to_string(rs->band_gsm);

		ofono_dbus_signal_property_changed(conn, path,
						OFONO_RADIO_SETTINGS_INTERFACE,
						"GsmBand", DBUS_TYPE_STRING,
						&str_band);

		if (rs->settings) {
			g_key_file_set_integer(rs->settings, SETTINGS_GROUP,
					"GsmBand", rs->band_gsm);
			storage_sync(rs->imsi, SETTINGS_STORE, rs->settings);
		}
	}

	if (rs->band_umts != rs->pending_band_umts) {
		rs->band_umts = rs->pending_band_umts;
		str_band = radio_band_umts_to_string(rs->band_umts);

		ofono_dbus_signal_property_changed(conn, path,
						OFONO_RADIO_SETTINGS_INTERFACE,
						"UmtsBand", DBUS_TYPE_STRING,
						&str_band);

		if (rs->settings) {
			g_key_file_set_integer(rs->settings, SETTINGS_GROUP,
					"UmtsBand", rs->band_umts);
			storage_sync(rs->imsi, SETTINGS_STORE, rs->settings);
		}
	}
}

static void radio_band_set_callback(const struct ofono_error *error,
					void *data)
{
	struct ofono_radio_settings *rs = data;

	if (error->type != OFONO_ERROR_TYPE_NO_ERROR) {
		DBG("Error setting radio frequency band");

		rs->pending_band_gsm = rs->band_gsm;
		rs->pending_band_umts = rs->band_umts;

		__ofono_dbus_queue_reply_failed(rs->q);

		return;
	}

	__ofono_dbus_queue_reply_ok(rs->q);

	radio_set_band(rs);
}

static void radio_set_rat_mode(struct ofono_radio_settings *rs, int mode)
{
	DBusConnection *conn = ofono_dbus_get_connection();
	const char *path;
	const char *str_mode;

	if (rs->mode == mode)
		return;

	rs->mode = mode;

	path = __ofono_atom_get_path(rs->atom);
	str_mode = radio_access_mode_to_string(rs->mode);

	ofono_dbus_signal_property_changed(conn, path,
						OFONO_RADIO_SETTINGS_INTERFACE,
						"TechnologyPreference",
						DBUS_TYPE_STRING, &str_mode);

	if (rs->settings) {
		g_key_file_set_string(rs->settings, SETTINGS_GROUP,
				"TechnologyPreference", str_mode);
		storage_sync(rs->imsi, SETTINGS_STORE, rs->settings);
	}
}

static void radio_mode_set_callback(const struct ofono_error *error, void *data)
{
	struct ofono_radio_settings *rs = data;

	if (error->type != OFONO_ERROR_TYPE_NO_ERROR) {
		DBG("Error setting radio access mode");

		rs->pending_mode = rs->mode;

		__ofono_dbus_queue_reply_failed(rs->q);

		return;
	}

	__ofono_dbus_queue_reply_ok(rs->q);

	radio_set_rat_mode(rs, rs->pending_mode);
}

static DBusMessage *radio_get_properties_reply_cb(DBusMessage *msg, void *data)
{
	return radio_get_properties_reply(msg, data);
}

static void radio_send_properties_reply(struct ofono_radio_settings *rs)
{
	rs->flags |= RADIO_SETTINGS_FLAG_CACHED;

	__ofono_dbus_queue_reply_all_fn_param(rs->q,
					radio_get_properties_reply_cb, rs);
}

static void radio_available_rats_query_callback(const struct ofono_error *error,
						unsigned int available_rats,
						void *data)
{
	struct ofono_radio_settings *rs = data;

	if (error->type == OFONO_ERROR_TYPE_NO_ERROR) {
		unsigned int m, n;

		/* Count number of bits */
		available_rats &= OFONO_RADIO_ACCESS_MODE_ALL;
		for (m = available_rats, n = 0; m; n++)
			m &= m - 1;

		g_free(rs->available_modes);
		rs->available_modes = g_new(enum ofono_radio_access_mode, n+1);

		for (m = available_rats, n = 0; m; n++) {
			/* Extract the least significant bit from the mask */
			rs->available_modes[n] = (m & ~(m - 1));
			m &= m - 1;
		}
		rs->available_modes[n] = 0;
	} else {
		DBG("Error while querying available rats");
	}

	radio_send_properties_reply(rs);
}

static void radio_available_modes_query_callback(const struct ofono_error *err,
			const enum ofono_radio_access_mode *modes, void *data)
{
	struct ofono_radio_settings *rs = data;

	if (err->type == OFONO_ERROR_TYPE_NO_ERROR) {
		unsigned int n;

		for (n = 0; modes[n]; n++);

		g_free(rs->available_modes);
		rs->available_modes = g_new(enum ofono_radio_access_mode, n+1);
		rs->available_modes[n] = 0;
		memcpy(rs->available_modes, modes, sizeof(modes[0]) * n);
	} else {
		DBG("Error while querying available modes");
	}

	radio_send_properties_reply(rs);
}

static void radio_query_available_rats(struct ofono_radio_settings *rs)
{
	/* Modem technology is not supposed to change, so one query is enough */
	if (rs->available_modes || (!rs->driver->query_available_rats &&
				!rs->driver->query_available_rat_modes)) {
		radio_send_properties_reply(rs);
		return;
	}

	if (rs->driver->query_available_rat_modes)
		rs->driver->query_available_rat_modes(
				rs, radio_available_modes_query_callback, rs);
	else
		rs->driver->query_available_rats(
				rs, radio_available_rats_query_callback, rs);
}

static void radio_fast_dormancy_query_callback(const struct ofono_error *error,
						ofono_bool_t enable, void *data)
{
	struct ofono_radio_settings *rs = data;

	if (error->type != OFONO_ERROR_TYPE_NO_ERROR) {
		DBG("Error during fast dormancy query");

		__ofono_dbus_queue_reply_failed(rs->q);

		return;
	}

	radio_set_fast_dormancy(rs, enable);
	radio_query_available_rats(rs);
}

static void radio_query_fast_dormancy(struct ofono_radio_settings *rs)
{
	if (rs->driver->query_fast_dormancy == NULL) {
		radio_query_available_rats(rs);
		return;
	}

	rs->driver->query_fast_dormancy(rs, radio_fast_dormancy_query_callback,
					rs);
}

static void radio_band_query_callback(const struct ofono_error *error,
					enum ofono_radio_band_gsm band_gsm,
					enum ofono_radio_band_umts band_umts,
					void *data)
{
	struct ofono_radio_settings *rs = data;

	if (error->type != OFONO_ERROR_TYPE_NO_ERROR) {
		DBG("Error during radio frequency band query");

		__ofono_dbus_queue_reply_failed(rs->q);

		return;
	}

	rs->pending_band_gsm = band_gsm;
	rs->pending_band_umts = band_umts;

	radio_set_band(rs);
	radio_query_fast_dormancy(rs);
}

static void radio_query_band(struct ofono_radio_settings *rs)
{
	if (rs->driver->query_band == NULL) {
		radio_query_fast_dormancy(rs);
		return;
	}

	rs->driver->query_band(rs, radio_band_query_callback, rs);
}

static void radio_rat_mode_query_callback(const struct ofono_error *error,
					enum ofono_radio_access_mode rat,
					void *data)
{
	struct ofono_radio_settings *rs = data;
	int mode = rat;

	if (error->type != OFONO_ERROR_TYPE_NO_ERROR) {
		DBG("Error during radio access mode query");

		__ofono_dbus_queue_reply_failed(rs->q);

		return;
	}

	if (radio_legacy_rat_driver(rs)) {
		mode = -mode;
	} else {
		/*
		 * If we have previously set a legacy mode, let's check
		 * it if still maps to the current (real) mode and
		 * if it does, keep the legacy mode. This is necessary
		 * for compatibility with legacy UIs that only know
		 * about legacy modes.
		 */
		if (rs->mode < 0 &&
			radio_map_legacy_rat(rs, -rs->mode) == mode) {
			mode = rs->mode;
		}
	}

	radio_set_rat_mode(rs, mode);
	radio_query_band(rs);
}

static DBusMessage *radio_get_properties_handler(DBusMessage *msg, void *data)
{
	struct ofono_radio_settings *rs = data;

	if (rs->flags & RADIO_SETTINGS_FLAG_CACHED)
		return radio_get_properties_reply(msg, rs);

	if (rs->driver->query_rat_mode == NULL)
		return __ofono_error_not_implemented(msg);

	rs->driver->query_rat_mode(rs, radio_rat_mode_query_callback, rs);

	return NULL;
}

static DBusMessage *radio_set_property_handler(DBusMessage *msg, void *data)
{
	struct ofono_radio_settings *rs = data;
	DBusMessageIter iter;
	DBusMessageIter var;
	const char *property;

	if (!dbus_message_iter_init(msg, &iter))
		return __ofono_error_invalid_args(msg);

	if (dbus_message_iter_get_arg_type(&iter) != DBUS_TYPE_STRING)
		return __ofono_error_invalid_args(msg);

	dbus_message_iter_get_basic(&iter, &property);
	dbus_message_iter_next(&iter);

	if (!__ofono_dbus_access_method_allowed(dbus_message_get_sender(msg),
		OFONO_DBUS_ACCESS_INTF_RADIOSETTINGS,
		OFONO_DBUS_ACCESS_RADIOSETTINGS_SET_PROPERTY, property))
		return __ofono_error_access_denied(msg);

	if (dbus_message_iter_get_arg_type(&iter) != DBUS_TYPE_VARIANT)
		return __ofono_error_invalid_args(msg);

	dbus_message_iter_recurse(&iter, &var);

	if (g_strcmp0(property, "TechnologyPreference") == 0) {
		const char *value;
		int mode;
		enum ofono_radio_access_mode am;

		if (rs->driver->set_rat_mode == NULL)
			return __ofono_error_not_implemented(msg);

		if (dbus_message_iter_get_arg_type(&var) != DBUS_TYPE_STRING)
			return __ofono_error_invalid_args(msg);

		dbus_message_iter_get_basic(&var, &value);
		if (radio_legacy_mode_from_string(value, &am)) {
			mode = -(int)am;

			if (radio_legacy_rat_driver(rs)) {
				/* Make sure it's supported */
				if (!radio_access_mode_is_supported(rs, am))
					return __ofono_error_not_supported(msg);
			} else if (mode != OFONO_RADIO_ACCESS_MODE_ANY) {
				/* Map a legacy value into the real one */
				am = radio_map_legacy_rat(rs, am);
				if (!am)
					return __ofono_error_not_supported(msg);
			}
		} else if (radio_access_modes_from_string(value, &am)) {
			mode = am;

			/* Make sure this combination of modes is supported */
			if (radio_legacy_rat_driver(rs) ||
				!radio_access_mode_is_supported(rs, am))
				return __ofono_error_not_supported(msg);
		} else
			return __ofono_error_invalid_args(msg);

		if (rs->mode == mode)
			return dbus_message_new_method_return(msg);

		rs->pending_mode = mode;

		rs->driver->set_rat_mode(rs, am, radio_mode_set_callback, rs);
		/* will be saved in radiosettng on success response*/
		return NULL;
	} else if (g_strcmp0(property, "GsmBand") == 0) {
		const char *value;
		enum ofono_radio_band_gsm band;

		if (rs->driver->set_band == NULL)
			return __ofono_error_not_implemented(msg);

		if (dbus_message_iter_get_arg_type(&var) != DBUS_TYPE_STRING)
			return __ofono_error_invalid_args(msg);

		dbus_message_iter_get_basic(&var, &value);
		if (radio_band_gsm_from_string(value, &band) == FALSE)
			return __ofono_error_invalid_args(msg);

		if (rs->band_gsm == band)
			return dbus_message_new_method_return(msg);

		rs->pending_band_gsm = band;

		rs->driver->set_band(rs, band, rs->band_umts,
					radio_band_set_callback, rs);
		/* will be saved in radiosettng on success response*/
		return NULL;
	} else if (g_strcmp0(property, "UmtsBand") == 0) {
		const char *value;
		enum ofono_radio_band_umts band;

		if (rs->driver->set_band == NULL)
			return __ofono_error_not_implemented(msg);

		if (dbus_message_iter_get_arg_type(&var) != DBUS_TYPE_STRING)
			return __ofono_error_invalid_args(msg);

		dbus_message_iter_get_basic(&var, &value);
		if (radio_band_umts_from_string(value, &band) == FALSE)
			return __ofono_error_invalid_args(msg);

		if (rs->band_umts == band)
			return dbus_message_new_method_return(msg);

		rs->pending_band_umts = band;

		rs->driver->set_band(rs, rs->band_gsm, band,
					radio_band_set_callback, rs);
		/* will be saved in radiosettng on success response*/
		return NULL;
	} else if (g_strcmp0(property, "FastDormancy") == 0) {
		dbus_bool_t value;
		int target;

		if (rs->driver->set_fast_dormancy == NULL)
			return __ofono_error_not_implemented(msg);

		if (dbus_message_iter_get_arg_type(&var) != DBUS_TYPE_BOOLEAN)
			return __ofono_error_invalid_args(msg);

		dbus_message_iter_get_basic(&var, &value);
		target = value;

		if (rs->fast_dormancy_pending == target)
			return dbus_message_new_method_return(msg);

		rs->fast_dormancy_pending = target;

		rs->driver->set_fast_dormancy(rs, target,
					radio_fast_dormancy_set_callback, rs);
		return NULL;
	}

	return __ofono_error_invalid_args(msg);
}

static DBusMessage *radio_get_properties(DBusConnection *conn,
						DBusMessage *msg, void *data)
{
	struct ofono_radio_settings *rs = data;

	__ofono_dbus_queue_request(rs->q, radio_get_properties_handler,
								msg, rs);
	return NULL;
}

static DBusMessage *radio_set_property(DBusConnection *conn, DBusMessage *msg,
					void *data)
{
	struct ofono_radio_settings *rs = data;

	__ofono_dbus_queue_request(rs->q, radio_set_property_handler,
								msg, rs);
	return NULL;
}

static const GDBusMethodTable radio_methods[] = {
	{ GDBUS_ASYNC_METHOD("GetProperties",
			NULL, GDBUS_ARGS({ "properties", "a{sv}" }),
			radio_get_properties) },
	{ GDBUS_ASYNC_METHOD("SetProperty",
			GDBUS_ARGS({ "property", "s" }, { "value", "v" }),
			NULL, radio_set_property) },
	{ }
};

static const GDBusSignalTable radio_signals[] = {
	{ GDBUS_SIGNAL("PropertyChanged",
			GDBUS_ARGS({ "name", "s" }, { "value", "v" })) },
	{ }
};

int ofono_radio_settings_driver_register(const struct ofono_radio_settings_driver *d)
{
	DBG("driver: %p, name: %s", d, d->name);

	if (d == NULL || d->probe == NULL)
		return -EINVAL;

	g_drivers = g_slist_prepend(g_drivers, (void *) d);

	return 0;
}

void ofono_radio_settings_driver_unregister(const struct ofono_radio_settings_driver *d)
{
	DBG("driver: %p, name: %s", d, d->name);

	if (d == NULL)
		return;

	g_drivers = g_slist_remove(g_drivers, (void *) d);
}

static void radio_settings_unregister(struct ofono_atom *atom)
{
	struct ofono_radio_settings *rs = __ofono_atom_get_data(atom);
	const char *path = __ofono_atom_get_path(rs->atom);
	DBusConnection *conn = ofono_dbus_get_connection();
	struct ofono_modem *modem = __ofono_atom_get_modem(rs->atom);

	__ofono_dbus_queue_free(rs->q);
	g_free(rs->available_modes);
	ofono_modem_remove_interface(modem, OFONO_RADIO_SETTINGS_INTERFACE);
	g_dbus_unregister_interface(conn, path, OFONO_RADIO_SETTINGS_INTERFACE);

	if (rs->settings) {
		storage_close(rs->imsi, SETTINGS_STORE, rs->settings, TRUE);

		g_free(rs->imsi);
		rs->imsi = NULL;
		rs->settings = NULL;
	}
}

static void radio_settings_remove(struct ofono_atom *atom)
{
	struct ofono_radio_settings *rs = __ofono_atom_get_data(atom);

	DBG("atom: %p", atom);

	if (rs == NULL)
		return;

	if (rs->driver && rs->driver->remove)
		rs->driver->remove(rs);

	g_free(rs);
}

struct ofono_radio_settings *ofono_radio_settings_create(struct ofono_modem *modem,
							unsigned int vendor,
							const char *driver,
							void *data)
{
	struct ofono_radio_settings *rs;
	GSList *l;

	if (driver == NULL)
		return NULL;

	rs = g_try_new0(struct ofono_radio_settings, 1);
	if (rs == NULL)
		return NULL;

	rs->mode = 0;
	rs->q = __ofono_dbus_queue_new();
	rs->atom = __ofono_modem_add_atom(modem, OFONO_ATOM_TYPE_RADIO_SETTINGS,
						radio_settings_remove, rs);

	for (l = g_drivers; l; l = l->next) {
		const struct ofono_radio_settings_driver *drv = l->data;

		if (g_strcmp0(drv->name, driver) != 0)
			continue;

		if (drv->probe(rs, vendor, data) < 0)
			continue;

		rs->driver = drv;
		break;
	}

	return rs;
}

static void ofono_radio_finish_register(struct ofono_radio_settings *rs)
{
	DBusConnection *conn = ofono_dbus_get_connection();
	struct ofono_modem *modem = __ofono_atom_get_modem(rs->atom);
	const char *path = __ofono_atom_get_path(rs->atom);

	if (!g_dbus_register_interface(conn, path,
					OFONO_RADIO_SETTINGS_INTERFACE,
					radio_methods, radio_signals,
					NULL, rs, NULL)) {
		ofono_error("Could not create %s interface",
				OFONO_RADIO_SETTINGS_INTERFACE);
		return;
	}

	ofono_modem_add_interface(modem, OFONO_RADIO_SETTINGS_INTERFACE);

	__ofono_atom_register(rs->atom, radio_settings_unregister);
}

static void radio_mode_set_callback_at_reg(const struct ofono_error *error,
						void *data)
{
	struct ofono_radio_settings *rs = data;

	if (error->type != OFONO_ERROR_TYPE_NO_ERROR)
		DBG("Error setting radio access mode register time");

	/*
	 * Continue with atom register even if request fail at modem
	 */
	ofono_radio_finish_register(rs);
}

static void radio_band_set_callback_at_reg(const struct ofono_error *error,
						void *data)
{
	if (error->type != OFONO_ERROR_TYPE_NO_ERROR)
		DBG("Error setting radio access mode register time");
	/*
	 * Continue with atom register even if request fail at modem
	 * ofono_radio_finish_register called by radio_mode_set_callback_at_reg
	 */
}

static void radio_load_settings(struct ofono_radio_settings *rs,
					const char *imsi)
{
	GError *error;
	char *str;
	gboolean save_mode = FALSE;

	rs->settings = storage_open(imsi, SETTINGS_STORE);

	/*
	 * If no settings present or error; Set default.
	 * Default RAT mode: ANY (LTE > UMTS > GSM)
	 */
	if (rs->settings == NULL) {
		DBG("radiosetting storage open failed");
		rs->mode = OFONO_RADIO_ACCESS_MODE_ANY;
		rs->band_gsm = OFONO_RADIO_BAND_GSM_ANY;
		rs->band_umts = OFONO_RADIO_BAND_UMTS_ANY;
		return;
	}

	rs->imsi = g_strdup(imsi);

	error = NULL;
	rs->band_gsm = g_key_file_get_integer(rs->settings, SETTINGS_GROUP,
					"GsmBand", &error);

	if (error || radio_band_gsm_to_string(rs->band_gsm) == NULL) {
		rs->band_gsm = OFONO_RADIO_BAND_GSM_ANY;
		g_key_file_set_integer(rs->settings, SETTINGS_GROUP,
						"GsmBand", rs->band_gsm);
	}

	if (error) {
		g_error_free(error);
		error = NULL;
	}

	rs->pending_band_gsm = rs->band_gsm;

	rs->band_umts = g_key_file_get_integer(rs->settings, SETTINGS_GROUP,
					"UmtsBand", &error);

	if (error || radio_band_umts_to_string(rs->band_umts) == NULL) {
		rs->band_umts = OFONO_RADIO_BAND_UMTS_ANY;
		g_key_file_set_integer(rs->settings, SETTINGS_GROUP,
						"UmtsBand", rs->band_umts);
	}

	if (error) {
		g_error_free(error);
		error = NULL;
	}

	rs->pending_band_umts = rs->band_umts;

	rs->mode = 0;

	str = g_key_file_get_string(rs->settings, SETTINGS_GROUP,
					"TechnologyPreference", NULL);

	if (str) {
		enum ofono_radio_access_mode am;

		if (radio_legacy_mode_from_string(str, &am)) {
			if (radio_legacy_rat_driver(rs) ||
					radio_map_legacy_rat(rs, am)) {
				rs->mode = -(int)am;
			}
		} else if (radio_access_modes_from_string(str, &am)) {
			/* Mask of rats */
			rs->mode = am;
		} else {
			/* Old format (integer) */
			save_mode = TRUE;
			rs->mode = -atoi(str);
			DBG("migrating %s -> %s", str,
				radio_access_mode_to_string(rs->mode));
		}
		g_free(str);
	}

	if (save_mode) {
		g_key_file_set_string(rs->settings, SETTINGS_GROUP,
				"TechnologyPreference",
				radio_access_mode_to_string(rs->mode));
		/* No need to save the file right away */
	}

	if (error) {
		g_error_free(error);
		error = NULL;
	}

	DBG("TechnologyPreference: %s", radio_access_mode_to_string(rs->mode));
	DBG("GsmBand: %d", rs->band_gsm);
	DBG("UmtsBand: %d", rs->band_umts);
}

void ofono_radio_settings_register(struct ofono_radio_settings *rs)
{
	struct ofono_modem *modem = __ofono_atom_get_modem(rs->atom);
	struct ofono_sim *sim = __ofono_atom_find(OFONO_ATOM_TYPE_SIM, modem);

	if (sim == NULL)
		goto finish;

	radio_load_settings(rs, ofono_sim_get_imsi(sim));

	if (rs->driver->set_band != NULL)
		rs->driver->set_band(rs, rs->band_gsm, rs->band_umts,
					radio_band_set_callback_at_reg, rs);

	if (rs->driver->set_rat_mode == NULL)
		goto finish;

	/*
	 * Diff callback used. No need of using DBUS pending concept.
	 * As its atom registration time - no DBUS clients.
	 */
	rs->driver->set_rat_mode(rs, rs->mode < 0 ?
			radio_map_legacy_rat(rs, -rs->mode) : rs->mode,
					radio_mode_set_callback_at_reg, rs);
	return;

finish:
	ofono_radio_finish_register(rs);
}

void ofono_radio_settings_remove(struct ofono_radio_settings *rs)
{
	__ofono_atom_free(rs->atom);
}

void ofono_radio_settings_set_data(struct ofono_radio_settings *rs,
					void *data)
{
	rs->driver_data = data;
}

void *ofono_radio_settings_get_data(struct ofono_radio_settings *rs)
{
	return rs->driver_data;
}

struct ofono_modem *ofono_radio_settings_get_modem(
					struct ofono_radio_settings *rs)
{
	return __ofono_atom_get_modem(rs->atom);
}
