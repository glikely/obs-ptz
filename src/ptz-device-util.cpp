/* Pan Tilt Zoom Controls - what the controller does with a device's filter
 *
 * Copyright 2026 Grant Likely <grant.likely@secretlab.ca>
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * The controller's half of the functions ptz.h declares for devices. It
 * knows a device only through the PTZ API and OBS, not through PTZDevice.
 */

#include <obs.hpp>
#include <QString>
#include <qt-wrappers.hpp>
#include "ptz.h"

bool ptz_source_is_device(obs_source_t *source)
{
	proc_handler_t *ph = source ? obs_source_get_proc_handler(source) : nullptr;
	if (!ph)
		return false;
	calldata_t cd = {};
	bool usable = proc_handler_call(ph, "ptz_get_api_version", &cd) &&
		      calldata_int(&cd, "major") == PTZ_API_VERSION_MAJOR &&
		      calldata_int(&cd, "minor") >= PTZ_API_VERSION_MINOR;
	calldata_free(&cd);
	return usable;
}

obs_source_t *ptz_device_create_filter(obs_source_t *parent, obs_data_t *config)
{
	const char *kind = obs_data_get_string(config, "kind");
	if (!*kind)
		kind = ptz_legacy_kind_for_type(obs_data_get_string(config, "type"));
	if (!parent || !kind)
		return nullptr;
	/* OBS would make a filter of a kind no module registered, which is no device */
	OBSDataAutoRelease kindDefaults = obs_get_source_defaults(kind);
	if (!kindDefaults)
		return nullptr;

	/* The filter knows its own source, and a device id isn't stable across
	 * a driver change (as PTZDevice::stripIdentity() and ptz_filter_save()
	 * have it); the rest is the backup's, or a default */
	OBSDataAutoRelease settings = obs_data_create();
	obs_data_apply(settings, config);
	obs_data_erase(settings, "name");
	obs_data_erase(settings, "id");
	obs_data_erase(settings, "is-self-managed");
	obs_data_erase(settings, "backup_time");
	obs_data_erase(settings, "kind");
	obs_data_erase(settings, PTZ_DEFAULTS_MAJOR_KEY);
	obs_data_erase(settings, PTZ_DEFAULTS_MINOR_KEY);

	QString base = QT_UTF8(obs_source_get_display_name(kind));
	QString name = base;
	for (int i = 2;; i++) {
		OBSSourceAutoRelease existing = obs_source_get_filter_by_name(parent, QT_TO_UTF8(name));
		if (!existing)
			break;
		name = QString("%1 %2").arg(base).arg(i);
	}

	obs_source_t *filter = obs_source_create(kind, QT_TO_UTF8(name), settings, nullptr);
	if (filter)
		obs_source_filter_add(parent, filter);
	return filter;
}
