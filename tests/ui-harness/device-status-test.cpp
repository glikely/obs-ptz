/* PTZ UI test harness: device connection/position status test
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

/* Reports a device's live PTZDevice::isConnected() state plus its
 * cached "pan_pos"/"tilt_pos" properties (populated from the camera's
 * own inquiry replies -- see PTZVisca::receive() in src/ptz-visca.cpp --
 * and readable generically through the "ptz_get" proc handler, see
 * PTZDevice::get() in src/ptz-device.cpp) as JSON.
 *
 * obs-websocket exposes no device list or property read of its own (see
 * conftest.py's own comment on DEVICE_IDS), so this is the only way
 * tests/obs-integration/test_device_status.py can observe camera
 * connect/disconnect and position-update behavior from outside the
 * plugin. */
void runDeviceStatusTest(const QMap<QString, QString> &params)
{
	bool deviceIdOk = false;
	uint32_t deviceId = params.value(QStringLiteral("device_id")).toUInt(&deviceIdOk);
	QString filename = params.value(QStringLiteral("filename"));
	if (!deviceIdOk || filename.isEmpty()) {
		blog(LOG_INFO, "[ptz-ui-test] get_device_status: missing/invalid device_id or filename");
		return;
	}

	QModelIndex index = ptzDeviceList.indexFromDeviceId(deviceId);
	PTZDevice *ptz = ptzDeviceList.getDevice(index);
	if (!ptz) {
		blog(LOG_INFO, "[ptz-ui-test] get_device_status: device_id %u not found", deviceId);
		return;
	}

	calldata cd = {};
	calldata_set_string(&cd, "property", "pan_pos");
	ptzDeviceList.callDevice(index, "ptz_get", &cd);
	long long pan_pos = calldata_int(&cd, "pan_pos");

	calldata_set_string(&cd, "property", "tilt_pos");
	ptzDeviceList.callDevice(index, "ptz_get", &cd);
	long long tilt_pos = calldata_int(&cd, "tilt_pos");

	calldata_free(&cd);

	OBSDataAutoRelease result = obs_data_create();
	obs_data_set_bool(result, "connected", ptz->isConnected());
	obs_data_set_int(result, "pan_pos", pan_pos);
	obs_data_set_int(result, "tilt_pos", tilt_pos);

	if (!obs_data_save_json_safe(result, qUtf8Printable(filename), "tmp", "bak"))
		blog(LOG_INFO, "[ptz-ui-test] get_device_status: failed to write %s", qUtf8Printable(filename));
}

} // namespace

/* Request params:
 *   device_id - the target device's numeric id (PTZDevice::id, same
 *               space tests/obs-integration/conftest.py's device_ids
 *               uses)
 *   filename  - where to write the {"connected", "pan_pos", "tilt_pos"}
 *               JSON result
 */
void registerDeviceStatusTest(PTZUITestHarness *harness)
{
	harness->registerTest(QStringLiteral("get_device_status"), &runDeviceStatusTest);
}
