/* Pan Tilt Zoom Controls - the PTZ API's procs and signals, as tables
 *
 * Copyright 2026 Grant Likely <grant.likely@secretlab.ca>
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * Every proc and signal docs/ptz-device-api.md specifies, with the declaration
 * it is registered by and, for a proc, what a caller outside OBS has to be let
 * do to call it, in one table for each handler section of the doc. They are
 * fixed when the plugin is built, and are the same in every module that links
 * them: nothing is added to them as devices are made. libobs can't be asked
 * what a handler has, so this is how the plugin knows, the websocket binding
 * above all. They are the plugin's own, and not part of the API. They need
 * neither OBS nor Qt.
 */
#pragma once

#include <cstddef>
#include <cstring>

/* What docs/ptz-device-api.md says of a proc: what a caller from outside OBS
 * has to be let do to call it, read the device, control it, or nothing, for a
 * proc that can't be called from there. Every proc may be called from any
 * thread. */
enum PTZProcAccess { PTZ_ACCESS_CONTROL, PTZ_ACCESS_READ, PTZ_ACCESS_NONE };

struct PTZApiProc {
	/* "ptz_preset_recall" */
	const char *name;
	/* "void ptz_preset_recall(string id)" */
	const char *decl;
	PTZProcAccess access;
};

struct PTZApiSignal {
	/* "ptz_preset_recalled" */
	const char *name;
	/* "void ptz_preset_recalled(ptr source, string id)" */
	const char *decl;
};

/* One table, for a range-for */
template<typename Entry> struct PTZApiTable {
	const Entry *first;
	size_t count;
	const Entry *begin() const { return first; }
	const Entry *end() const { return first + count; }

	/* The entry named `name`, or null */
	const Entry *find(const char *name) const
	{
		for (const Entry &entry : *this)
			if (strcmp(entry.name, name) == 0)
				return &entry;
		return nullptr;
	}
};

using PTZApiProcs = PTZApiTable<PTZApiProc>;
using PTZApiSignals = PTZApiTable<PTZApiSignal>;

/* The doc's four handler sections, in its order */
PTZApiProcs ptz_api_device_procs();
PTZApiSignals ptz_api_device_signals();
PTZApiProcs ptz_api_discovery_procs();
PTZApiSignals ptz_api_discovery_signals();

/* "read", "control" or "none", as the websocket binding and its clients have it */
const char *ptz_api_access_name(PTZProcAccess access);
