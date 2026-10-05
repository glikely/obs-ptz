/* PTZ UI test harness: the PTZ API's version
 *
 * Copyright 2026 Grant Likely <grant.likely@secretlab.ca>
 *
 * SPDX-License-Identifier: GPLv2
 */
#include "ui-test-harness.hpp"

#include <obs.hpp>
#include <obs-module.h>

#include "ptz-device.hpp"
#include "ptz-list-model.hpp"

namespace {

/* Asks a device for the version through its ptz_get_api_version, the way
 * another plugin would */
void runGetApiVersionTest(const QMap<QString, QString> &params)
{
	QString filename = params.value(QStringLiteral("filename"));
	if (filename.isEmpty()) {
		blog(LOG_INFO, "[ptz-ui-test] get_api_version: missing filename");
		return;
	}

	calldata_t cd = {};
	bool called = ptzDeviceList->callDevice(ptzUITestDeviceIndex(params.value(QStringLiteral("device"))),
						"ptz_get_api_version", &cd);
	OBSDataAutoRelease result = obs_data_create();
	obs_data_set_bool(result, "called", called);
	obs_data_set_int(result, "major", calldata_int(&cd, "major"));
	obs_data_set_int(result, "minor", calldata_int(&cd, "minor"));
	calldata_free(&cd);

	if (!obs_data_save_json_safe(result, qUtf8Printable(filename), "tmp", "bak"))
		blog(LOG_INFO, "[ptz-ui-test] get_api_version: failed to write %s", qUtf8Printable(filename));
}

/* Writes the procs and signals the plugin has registered, from the log that
 * ptz_proc_add() and ptz_signal_add() keep */
void runGetRegisteredApiTest(const QMap<QString, QString> &params)
{
	QString filename = params.value(QStringLiteral("filename"));
	if (filename.isEmpty()) {
		blog(LOG_INFO, "[ptz-ui-test] get_registered_api: missing filename");
		return;
	}

	OBSDataArrayAutoRelease registered = obs_data_array_create();
	for (const auto &[scope, signature] : ptz_registered_api()) {
		OBSDataAutoRelease item = obs_data_create();
		obs_data_set_string(item, "scope", qUtf8Printable(scope));
		obs_data_set_string(item, "signature", qUtf8Printable(signature));
		obs_data_array_push_back(registered, item);
	}
	OBSDataAutoRelease result = obs_data_create();
	obs_data_set_array(result, "registered", registered);

	if (!obs_data_save_json_safe(result, qUtf8Printable(filename), "tmp", "bak"))
		blog(LOG_INFO, "[ptz-ui-test] get_registered_api: failed to write %s", qUtf8Printable(filename));
}

} // namespace

/* get_api_version request params:
 *   device - the device, by the UUID of its filter or the name of the source it is on
 *   filename  - where to write the {"called", "major", "minor"} JSON result;
 *               "called" is false if there was no such device, or no
 *               ptz_get_api_version to call
 */
void registerApiVersionTest(PTZUITestHarness *harness)
{
	harness->registerTest(QStringLiteral("get_api_version"), &runGetApiVersionTest);
	harness->registerTest(QStringLiteral("get_registered_api"), &runGetRegisteredApiTest);
}

/* get_registered_api request params:
 *   filename  - where to write the {"registered": [{"scope", "signature"}...]}
 *               JSON result; see ptz_registered_api() for the scopes
 */
