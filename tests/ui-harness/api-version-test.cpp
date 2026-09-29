/* PTZ UI test harness: the PTZ API's version
 *
 * Copyright 2026 Grant Likely <grant.likely@secretlab.ca>
 *
 * SPDX-License-Identifier: GPLv2
 */
#include "ui-test-harness.hpp"

#include <obs.hpp>
#include <obs-module.h>

#include "ptz-list-model.hpp"

namespace {

/* Asks for the version through ptz_get_api_version, the way another plugin
 * would: on OBS's own proc_handler, or on a device's if given one */
void runGetApiVersionTest(const QMap<QString, QString> &params)
{
	QString filename = params.value(QStringLiteral("filename"));
	if (filename.isEmpty()) {
		blog(LOG_INFO, "[ptz-ui-test] get_api_version: missing filename");
		return;
	}

	calldata_t cd = {};
	bool called;
	if (params.contains(QStringLiteral("device_id"))) {
		uint32_t deviceId = params.value(QStringLiteral("device_id")).toUInt();
		called = ptzDeviceList->callDevice(ptzDeviceList->indexFromDeviceId(deviceId), "ptz_get_api_version",
						   &cd);
	} else {
		called = proc_handler_call(obs_get_proc_handler(), "ptz_get_api_version", &cd);
	}
	OBSDataAutoRelease result = obs_data_create();
	obs_data_set_bool(result, "called", called);
	obs_data_set_int(result, "major", calldata_int(&cd, "major"));
	obs_data_set_int(result, "minor", calldata_int(&cd, "minor"));
	calldata_free(&cd);

	if (!obs_data_save_json_safe(result, qUtf8Printable(filename), "tmp", "bak"))
		blog(LOG_INFO, "[ptz-ui-test] get_api_version: failed to write %s", qUtf8Printable(filename));
}

} // namespace

/* get_api_version request params:
 *   device_id - optional, a device to ask instead of the plugin
 *   filename  - where to write the {"called", "major", "minor"} JSON result;
 *               "called" is false if there was no ptz_get_api_version to call
 */
void registerApiVersionTest(PTZUITestHarness *harness)
{
	harness->registerTest(QStringLiteral("get_api_version"), &runGetApiVersionTest);
}
