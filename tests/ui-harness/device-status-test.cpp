/* PTZ UI test harness: device connection status test
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

/* Reports a device's live connection state (PTZListModel::
 * IsConnectedRole, which wraps PTZDevice::isConnected() -- see
 * ptz-list-model.cpp) plus its cached
 * "power_on"/"focus_af_enabled"/"wb_mode" properties (populated from
 * the camera's own inquiry replies -- see
 * PTZVisca::receive() in src/ptz-visca.cpp -- and readable generically
 * through the "ptz_get" proc handler, see PTZDevice::get()/
 * PTZVisca::get() in src/ptz-device.cpp/src/ptz-visca.cpp) as JSON.
 * "wb_mode" is VISCA-specific (PTZDevice::get() doesn't know it), so it
 * always reads back 0 on non-VISCA devices. "connected" is the wire
 * link's own state, distinct from "power_on" (the camera's reported
 * power state over that link).
 *
 * obs-websocket exposes no device list or property read of its own (see
 * conftest.py's own comment on DEVICE_IDS), so this is the only way
 * tests/obs-integration/'s device-status-driven tests can observe
 * camera connect/disconnect, power, autofocus and white balance
 * behavior from outside the plugin. */
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
	if (!index.isValid()) {
		blog(LOG_INFO, "[ptz-ui-test] get_device_status: device_id %u not found", deviceId);
		return;
	}

	calldata cd = {};
	calldata_set_string(&cd, "property", "power_on");
	ptzDeviceList.callDevice(index, "ptz_get", &cd);
	bool power_on = calldata_bool(&cd, "power_on");

	calldata_set_string(&cd, "property", "focus_af_enabled");
	ptzDeviceList.callDevice(index, "ptz_get", &cd);
	bool focus_af_enabled = calldata_bool(&cd, "focus_af_enabled");

	calldata_set_string(&cd, "property", "wb_mode");
	ptzDeviceList.callDevice(index, "ptz_get", &cd);
	long long wb_mode = calldata_int(&cd, "wb_mode");

	calldata_free(&cd);

	OBSDataAutoRelease result = obs_data_create();
	obs_data_set_bool(result, "connected", ptzDeviceList.data(index, PTZListModel::IsConnectedRole).toBool());
	obs_data_set_bool(result, "power_on", power_on);
	obs_data_set_bool(result, "focus_af_enabled", focus_af_enabled);
	obs_data_set_int(result, "wb_mode", wb_mode);

	if (!obs_data_save_json_safe(result, qUtf8Printable(filename), "tmp", "bak"))
		blog(LOG_INFO, "[ptz-ui-test] get_device_status: failed to write %s", qUtf8Printable(filename));
}

} // namespace

/* Request params:
 *   device_id - the target device's numeric id (PTZDevice::id, same
 *               space tests/obs-integration/conftest.py's device_ids
 *               uses)
 *   filename  - where to write the {"connected", "power_on",
 *               "focus_af_enabled", "wb_mode"} JSON result
 */
void registerDeviceStatusTest(PTZUITestHarness *harness)
{
	harness->registerTest(QStringLiteral("get_device_status"), &runDeviceStatusTest);
}
