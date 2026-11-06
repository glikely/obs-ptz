/* Pan Tilt Zoom Controls - the PTZ API's procs and signals, as tables
 *
 * Copyright 2026 Grant Likely <grant.likely@secretlab.ca>
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "ptz-api.hpp"

namespace {

/* Each in the order of its section of docs/ptz-device-api.md, which
 * tests/obs-integration/test_api_doc.py holds to this */
const PTZApiProc kDeviceProcs[] = {
	{"ptz_get_api_version", "void ptz_get_api_version(out int major, out int minor)", PTZ_ACCESS_READ},
	{"ptz_stop", "void ptz_stop()", PTZ_ACCESS_CONTROL},
	{"ptz_home_recall", "void ptz_home_recall()", PTZ_ACCESS_CONTROL},
	{"ptz_home_save", "void ptz_home_save()", PTZ_ACCESS_CONTROL},
	{"ptz_move", "void ptz_move(float pan, float tilt, float zoom, float focus)", PTZ_ACCESS_CONTROL},
	{"ptz_move_abs", "void ptz_move_abs(float pan, float tilt, float zoom, float focus)", PTZ_ACCESS_CONTROL},
	{"ptz_move_rel", "void ptz_move_rel(float pan, float tilt)", PTZ_ACCESS_CONTROL},
	{"ptz_preset_save", "void ptz_preset_save(string id)", PTZ_ACCESS_CONTROL},
	{"ptz_preset_recall", "void ptz_preset_recall(string id)", PTZ_ACCESS_CONTROL},
	{"ptz_preset_capture_thumbnail", "void ptz_preset_capture_thumbnail(string id, bool if_arrived)",
	 PTZ_ACCESS_CONTROL},
	{"ptz_get_state", "ptr ptz_get_state(ptr state)", PTZ_ACCESS_READ},
	{"ptz_get_statistics", "ptr ptz_get_statistics(ptr statistics)", PTZ_ACCESS_READ},
	{"ptz_get_parent_source", "ptr ptz_get_parent_source()", PTZ_ACCESS_NONE},
	{"ptz_request_state", "void ptz_request_state(ptr state)", PTZ_ACCESS_CONTROL},
	{"ptz_trigger", "void ptz_trigger(string name)", PTZ_ACCESS_CONTROL},
	{"ptz_get_camera_report", "void ptz_get_camera_report(out string report)", PTZ_ACCESS_READ},
	{"ptz_preset_get_list", "ptr ptz_preset_get_list()", PTZ_ACCESS_READ},
	{"ptz_preset_get_thumbnail", "void ptz_preset_get_thumbnail(string id, out string mime, out string data)",
	 PTZ_ACCESS_READ},
	{"ptz_preset_create", "string ptz_preset_create(string name, string store)", PTZ_ACCESS_CONTROL},
	{"ptz_preset_delete", "void ptz_preset_delete(string id)", PTZ_ACCESS_CONTROL},
	{"ptz_preset_update", "void ptz_preset_update(string id, ptr changes)", PTZ_ACCESS_CONTROL},
	{"ptz_preset_move", "void ptz_preset_move(string id, int index)", PTZ_ACCESS_CONTROL},
	{"ptz_preset_refresh", "void ptz_preset_refresh()", PTZ_ACCESS_CONTROL},
};

const PTZApiSignal kDeviceSignals[] = {
	{"ptz_state_changed", "void ptz_state_changed(ptr source, ptr changed)"},
	{"ptz_preset_added", "void ptz_preset_added(ptr source, string id)"},
	{"ptz_preset_removed", "void ptz_preset_removed(ptr source, string id)"},
	{"ptz_preset_order_changed", "void ptz_preset_order_changed(ptr source)"},
	{"ptz_preset_changed", "void ptz_preset_changed(ptr source, string id, ptr changed)"},
	{"ptz_preset_list_reset", "void ptz_preset_list_reset(ptr source)"},
	{"ptz_preset_create_done", "void ptz_preset_create_done(ptr source, string request, string id)"},
	{"ptz_preset_recalled", "void ptz_preset_recalled(ptr source, string id)"},
};

const PTZApiProc kDiscoveryProcs[] = {
	{"ptz_get_api_version", "void ptz_get_api_version(out int major, out int minor)", PTZ_ACCESS_READ},
	{"ptz_discovery_start", "void ptz_discovery_start()", PTZ_ACCESS_CONTROL},
	{"ptz_discovery_stop", "void ptz_discovery_stop()", PTZ_ACCESS_CONTROL},
};

const PTZApiSignal kDiscoverySignals[] = {
	{"ptz_device_found",
	 "void ptz_device_found(ptr source, string kind, string id, string name, string address, ptr settings)"},
	{"ptz_discovery_finished", "void ptz_discovery_finished(ptr source)"},
};

} // namespace

template<typename Entry, size_t N> static PTZApiTable<Entry> table(const Entry (&entries)[N])
{
	return {entries, N};
}

PTZApiProcs ptz_api_device_procs()
{
	return table(kDeviceProcs);
}

PTZApiSignals ptz_api_device_signals()
{
	return table(kDeviceSignals);
}

PTZApiProcs ptz_api_discovery_procs()
{
	return table(kDiscoveryProcs);
}

PTZApiSignals ptz_api_discovery_signals()
{
	return table(kDiscoverySignals);
}

const char *ptz_api_access_name(PTZProcAccess access)
{
	switch (access) {
	case PTZ_ACCESS_READ:
		return "read";
	case PTZ_ACCESS_NONE:
		return "none";
	case PTZ_ACCESS_CONTROL:
		break;
	}
	return "control";
}
