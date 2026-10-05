/* PTZ UI test harness: devices from another plugin
 *
 * Copyright 2026 Grant Likely <grant.likely@secretlab.ca>
 *
 * SPDX-License-Identifier: GPLv2
 */
#include "ui-test-harness.hpp"

#include <obs-module.h>

#include "ptz.h"

namespace {

/* Two filters that stand in for a plugin other than this one that provides
 * PTZ devices. They use nothing of this plugin's but the version number it
 * documents: what the API says a device is, and no more. One speaks the
 * version the plugin does, which makes it a device; the other a major
 * version later, which does not. */
const char *const GOOD_ID = "ptz_test_third_party_device";
const char *const OTHER_VERSION_ID = "ptz_test_third_party_device_other_version";

struct ThirdPartyDevice {
	obs_source_t *source;
	int major;
	int minor;
};

void getApiVersion(void *data, calldata_t *cd)
{
	auto self = static_cast<ThirdPartyDevice *>(data);
	calldata_set_int(cd, "major", self->major);
	calldata_set_int(cd, "minor", self->minor);
}

/* What the always-present state keys of the API document say */
void getState(void *data, calldata_t *cd)
{
	auto self = static_cast<ThirdPartyDevice *>(data);
	auto state = static_cast<obs_data_t *>(calldata_ptr(cd, "state"));
	if (!state)
		return;
	obs_source_t *parent = obs_filter_get_parent(self->source);
	obs_data_set_bool(state, "connected", true);
	obs_data_set_bool(state, "live", false);
	obs_data_set_bool(state, "preview", false);
	obs_data_set_bool(state, "locked", false);
	obs_data_set_string(state, "source", parent ? obs_source_get_name(parent) : "");
	obs_data_t *features = obs_data_create();
	obs_data_set_obj(state, "features", features);
	obs_data_release(features);
}

void *createDevice(obs_data_t *, obs_source_t *source)
{
	auto self = new ThirdPartyDevice{source, PTZ_API_VERSION_MAJOR, PTZ_API_VERSION_MINOR};
	if (strcmp(obs_source_get_id(source), OTHER_VERSION_ID) == 0)
		self->major++;

	proc_handler_t *ph = obs_source_get_proc_handler(source);
	proc_handler_add(ph, "void ptz_get_api_version(out int major, out int minor)", getApiVersion, self);
	proc_handler_add(ph, "ptr ptz_get_state(ptr state)", getState, self);

	signal_handler_t *sh = obs_source_get_signal_handler(source);
	signal_handler_add(sh, "void ptz_state_changed(ptr filter, ptr changed)");
	signal_handler_add(sh, "void ptz_settings_changed(ptr filter)");
	signal_handler_add(sh, "void ptz_preset_inserted(ptr filter, int row)");
	signal_handler_add(sh, "void ptz_preset_removed(ptr filter, int row)");
	signal_handler_add(sh, "void ptz_preset_moved(ptr filter, int src_row, int dest_row)");
	signal_handler_add(sh, "void ptz_preset_renamed(ptr filter, int id)");
	signal_handler_add(sh, "void ptz_preset_thumbnail_changed(ptr filter, int id)");
	return self;
}

void destroyDevice(void *data)
{
	delete static_cast<ThirdPartyDevice *>(data);
}

const char *deviceName(void *)
{
	return "Test PTZ device (from another plugin)";
}

void registerFilter(const char *id)
{
	obs_source_info info = {};
	info.id = id;
	info.type = OBS_SOURCE_TYPE_FILTER;
	info.output_flags = OBS_SOURCE_VIDEO;
	info.get_name = deviceName;
	info.create = createDevice;
	info.destroy = destroyDevice;
	obs_register_source(&info);
}

} // namespace

/* Registers the two filter kinds, "ptz_test_third_party_device" and
 * "ptz_test_third_party_device_other_version", for a test to add to a source
 * with obs-websocket, and see whether the plugin lists a device */
void registerThirdPartyDeviceTest(PTZUITestHarness *)
{
	registerFilter(GOOD_ID);
	registerFilter(OTHER_VERSION_ID);
}
