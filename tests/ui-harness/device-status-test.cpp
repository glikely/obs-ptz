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
 * ptz-list-model.cpp) as JSON.
 *
 * obs-websocket exposes no device list or property read of its own (see
 * conftest.py's own comment on DEVICE_IDS), so this is the only way
 * tests/obs-integration/test_device_status.py can observe camera
 * connect/disconnect behavior from outside the plugin. */
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

	OBSDataAutoRelease result = obs_data_create();
	obs_data_set_bool(result, "connected", ptzDeviceList.data(index, PTZListModel::IsConnectedRole).toBool());

	if (!obs_data_save_json_safe(result, qUtf8Printable(filename), "tmp", "bak"))
		blog(LOG_INFO, "[ptz-ui-test] get_device_status: failed to write %s", qUtf8Printable(filename));
}

} // namespace

/* Request params:
 *   device_id - the target device's numeric id (PTZDevice::id, same
 *               space tests/obs-integration/conftest.py's device_ids
 *               uses)
 *   filename  - where to write the {"connected"} JSON result
 */
void registerDeviceStatusTest(PTZUITestHarness *harness)
{
	harness->registerTest(QStringLiteral("get_device_status"), &runDeviceStatusTest);
}
