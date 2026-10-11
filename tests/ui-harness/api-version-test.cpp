/* PTZ UI test harness: the PTZ API's version
 *
 * Copyright 2026 Grant Likely <grant.likely@secretlab.ca>
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include "ui-test-harness.hpp"

#include <obs.hpp>
#include <obs-module.h>

#include "ptz-driver-kinds.hpp"
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

/* Makes, and destroys again, one instance of each discovery kind, which
 * registers its procs and signals, so that a build with the harness logs them
 * as "[ptz-api]" lines (see ptz_proc_add()) as a device's are when it is made.
 * Writes the kinds it made one of. */
void runMakeDiscoveryInstancesTest(const QMap<QString, QString> &params)
{
	QString filename = params.value(QStringLiteral("filename"));
	if (filename.isEmpty()) {
		blog(LOG_INFO, "[ptz-ui-test] make_discovery_instances: missing filename");
		return;
	}

	OBSDataArrayAutoRelease made = obs_data_array_create();
	for (const auto &kind : ptz_discovery_kinds()) {
		OBSSourceAutoRelease instance =
			obs_source_create_private(qUtf8Printable(kind.kind), "ptz-ui-test", nullptr);
		if (!instance)
			continue;
		OBSDataAutoRelease item = obs_data_create();
		obs_data_set_string(item, "kind", qUtf8Printable(kind.kind));
		obs_data_array_push_back(made, item);
	}
	OBSDataAutoRelease result = obs_data_create();
	obs_data_set_array(result, "made", made);

	if (!obs_data_save_json_safe(result, qUtf8Printable(filename), "tmp", "bak"))
		blog(LOG_INFO, "[ptz-ui-test] make_discovery_instances: failed to write %s", qUtf8Printable(filename));
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
	harness->registerTest(QStringLiteral("make_discovery_instances"), &runMakeDiscoveryInstancesTest);
}

/* make_discovery_instances request params:
 *   filename  - where to write the {"made": [{"kind"}...]} JSON result
 */
