/* Pan Tilt Zoom Controls - finding the PTZ drivers by their OBS filter kinds,
 * and the kinds that discover devices for them
 *
 * Copyright 2026 Grant Likely <grant.likely@secretlab.ca>
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include "ptz-driver-kinds.hpp"

#include <obs-module.h>
#include <qt-wrappers.hpp>
#include "ptz.h"

/* The kinds `enumerate` lists, with ids ending in `suffix` if one is given,
 * whose defaults carry the marker with our major version and a minor version
 * at least as new */
static QList<PTZDriverKind> ptz_marked_kinds(bool (*enumerate)(size_t, const char **), const char *suffix)
{
	QList<PTZDriverKind> kinds;
	const char *id;
	for (size_t i = 0; enumerate(i, &id); i++) {
		if (suffix && !QByteArray(id).endsWith(suffix))
			continue;
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

QList<PTZDriverKind> ptz_driver_kinds()
{
	return ptz_marked_kinds(obs_enum_filter_types, nullptr);
}

QList<PTZDriverKind> ptz_discovery_kinds()
{
	return ptz_marked_kinds(obs_enum_input_types, ".discovery");
}
