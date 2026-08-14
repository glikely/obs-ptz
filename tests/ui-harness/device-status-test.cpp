/* PTZ UI test harness: device connection status test
 *
 * Copyright 2026 Grant Likely <grant.likely@secretlab.ca>
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include "ui-test-harness.hpp"

#include <obs.hpp>
#include <obs-module.h>

#include "ptz-list-model.hpp"

namespace {

/* Reports a device's live connection state (PTZListModel::
 * IsConnectedRole, which wraps PTZDevice::isConnected() -- see
 * ptz-list-model.cpp) plus its "power_on"/"focus_af_enabled"/"wb_mode"
 * state (populated from the camera's own inquiry replies -- see
 * PTZVisca::receive() in src/ptz-visca.cpp -- and read through the
 * "ptz_get_state" proc handler) as JSON. A value the device doesn't report
 * reads back as false/0: "wb_mode" is VISCA's alone. "connected" is the
 * wire link's own state, distinct from "power_on" (the camera's reported
 * power state over that link).
 *
 * obs-websocket exposes no device list or property read of its own (see
 * conftest.py's own comment on DEVICE_IDS), so this is the only way
 * tests/obs-integration/'s device-status-driven tests can observe
 * camera connect/disconnect, power, autofocus and white balance
 * behavior from outside the plugin. */
void runDeviceStatusTest(const QMap<QString, QString> &params)
{
	QString deviceName = params.value(QStringLiteral("device"));
	bool deviceOk = !deviceName.isEmpty();
	QString filename = params.value(QStringLiteral("filename"));
	if (!deviceOk || filename.isEmpty()) {
		blog(LOG_INFO, "[ptz-ui-test] get_device_status: missing/invalid device or filename");
		return;
	}

	QModelIndex index = ptzUITestDeviceIndex(deviceName);
	if (!index.isValid()) {
		blog(LOG_INFO, "[ptz-ui-test] get_device_status: device %s not found", qUtf8Printable(deviceName));
		return;
	}

	OBSDataAutoRelease state = obs_data_create();
	ptzDeviceList->saveState(index, state.Get());

	OBSDataAutoRelease result = obs_data_create();
	obs_data_set_bool(result, "connected", ptzDeviceList->data(index, PTZListModel::IsConnectedRole).toBool());
	obs_data_set_bool(result, "power_on", obs_data_get_bool(state, "power_on"));
	obs_data_set_bool(result, "focus_af_enabled", obs_data_get_bool(state, "focus_af_enabled"));
	obs_data_set_int(result, "wb_mode", obs_data_get_int(state, "wb_mode"));

	if (!obs_data_save_json_safe(result, qUtf8Printable(filename), "tmp", "bak"))
		blog(LOG_INFO, "[ptz-ui-test] get_device_status: failed to write %s", qUtf8Printable(filename));
}

} // namespace

/* Request params:
 *   device - the device, by the UUID of its filter or the name of the source it is on
 *   filename  - where to write the {"connected", "power_on",
 *               "focus_af_enabled", "wb_mode"} JSON result
 */
void registerDeviceStatusTest(PTZUITestHarness *harness)
{
	harness->registerTest(QStringLiteral("get_device_status"), &runDeviceStatusTest);
}
