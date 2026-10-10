/* Pan Tilt Zoom Controls - finding the PTZ drivers by their OBS filter kinds
 *
 * Copyright 2026 Grant Likely <grant.likely@secretlab.ca>
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include "ptz-driver-kinds.hpp"

#include <obs-module.h>
#include <qt-wrappers.hpp>
#include "ptz.h"

QList<PTZDriverKind> ptz_driver_kinds()
{
	QList<PTZDriverKind> kinds;
	const char *id;
	for (size_t i = 0; obs_enum_filter_types(i, &id); i++) {
		OBSDataAutoRelease defaults = obs_get_source_defaults(id);
		if (!defaults)
			continue;
		if (!obs_data_has_default_value(defaults, PTZ_DEFAULTS_MAJOR_KEY) ||
		    !obs_data_has_default_value(defaults, PTZ_DEFAULTS_MINOR_KEY))
			continue;
		if (obs_data_get_int(defaults, PTZ_DEFAULTS_MAJOR_KEY) != PTZ_API_VERSION_MAJOR ||
		    obs_data_get_int(defaults, PTZ_DEFAULTS_MINOR_KEY) < PTZ_API_VERSION_MINOR)
			continue;
		const char *name = obs_source_get_display_name(id);
		kinds.append({QT_UTF8(id), QT_UTF8(name ? name : id), OBSData(defaults.Get())});
	}
	return kinds;
}
