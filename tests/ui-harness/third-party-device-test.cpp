/* PTZ UI test harness: devices from another plugin
 *
 * Copyright 2026 Grant Likely <grant.likely@secretlab.ca>
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include "ui-test-harness.hpp"

#include <obs-module.h>

#include "ptz.h"

namespace {

/* Filters and a source that stand in for a plugin other than this one that
 * provides PTZ devices. They use nothing of this plugin's but the version
 * number it documents: what the API says a device is, and no more. Two speak
 * the version the plugin does, which makes them devices, one as a filter and
 * one as a source; another, a filter, a major version later, which does not. */
const char *const GOOD_ID = "ptz_test_third_party_device";
const char *const GOOD_SOURCE_ID = "ptz_test_third_party_source_device";
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
	/* The source a device is on: a filter's parent, a source's own */
	obs_source_t *on = obs_source_get_type(self->source) == OBS_SOURCE_TYPE_FILTER
				   ? obs_filter_get_parent(self->source)
				   : self->source;
	obs_data_set_bool(state, "connected", true);
	obs_data_set_string(state, "source", on ? obs_source_get_name(on) : "");
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
	signal_handler_add(sh, "void ptz_state_changed(ptr source, ptr changed)");
	signal_handler_add(sh, "void ptz_preset_added(ptr source, string id)");
	signal_handler_add(sh, "void ptz_preset_removed(ptr source, string id)");
	signal_handler_add(sh, "void ptz_preset_order_changed(ptr source)");
	signal_handler_add(sh, "void ptz_preset_changed(ptr source, string id, ptr changed)");
	signal_handler_add(sh, "void ptz_preset_list_reset(ptr source)");
	return self;
}

void destroyDevice(void *data)
{
	delete static_cast<ThirdPartyDevice *>(data);
}

/* A video source that is not a filter has to say how big it is, or OBS will not
 * register it */
uint32_t deviceSize(void *)
{
	return 64;
}

const char *deviceName(void *)
{
	return "Test PTZ device (from another plugin)";
}

/* Something that has a host of its own, for the plugin to take a camera's
 * from, as it does from a DistroAV NDI source: its "web_control_url" setting */
const char *hostSourceName(void *)
{
	return "Test source with a host (not NDI)";
}

void *createHostSource(obs_data_t *, obs_source_t *source)
{
	return source;
}

void destroyHostSource(void *) {}

/* Without an .update OBS does not tell anyone the source's settings changed */
void updateHostSource(void *, obs_data_t *) {}

void registerHostSource()
{
	obs_source_info info = {};
	info.id = "ndi_source";
	info.type = OBS_SOURCE_TYPE_INPUT;
	info.output_flags = OBS_SOURCE_VIDEO;
	info.get_name = hostSourceName;
	info.create = createHostSource;
	info.destroy = destroyHostSource;
	info.update = updateHostSource;
	info.get_width = deviceSize;
	info.get_height = deviceSize;
	obs_register_source(&info);
}

void registerDevice(const char *id, obs_source_type type)
{
	obs_source_info info = {};
	info.id = id;
	info.type = type;
	info.output_flags = OBS_SOURCE_VIDEO;
	info.get_name = deviceName;
	info.create = createDevice;
	info.destroy = destroyDevice;
	if (type == OBS_SOURCE_TYPE_INPUT) {
		info.get_width = deviceSize;
		info.get_height = deviceSize;
	}
	obs_register_source(&info);
}

} // namespace

/* Registers the kinds, "ptz_test_third_party_device" and
 * "ptz_test_third_party_device_other_version" for a filter, and
 * "ptz_test_third_party_source_device" for a source, for a test to add with
 * obs-websocket, and see whether the plugin lists a device */
void registerThirdPartyDeviceTest(PTZUITestHarness *)
{
	registerDevice(GOOD_ID, OBS_SOURCE_TYPE_FILTER);
	registerDevice(OTHER_VERSION_ID, OBS_SOURCE_TYPE_FILTER);
	registerDevice(GOOD_SOURCE_ID, OBS_SOURCE_TYPE_INPUT);
	/* ...and "ndi_source", to say a host, as the kind of that id does */
	registerHostSource();
}
