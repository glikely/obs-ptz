/* PTZ UI test harness: device property set test (autofocus, white balance)
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

/* Drives PTZDevice::set() (the "ptz_set" proc handler, see
 * src/ptz-device.cpp/src/ptz-visca.cpp) directly -- the same request
 * PTZControls::on_deviceList_customContextMenuRequested() and
 * PTZControls::getCurrentDeviceAutofocus() already make from inside the
 * UI (src/ptz-controls.cpp) -- for properties that have no
 * ptz_action_source action type of their own. PTZ_ACTION_POWER_OFF/
 * PTZ_ACTION_POWER_ON exist in its PTZ_ACTION_* enum
 * (src/ptz-action-source.c) but are never actually wired up in
 * ptz_action_source_do_action() (default: break;) or exposed in its own
 * "action" property list, so power is no more reachable over
 * obs-websocket than autofocus/white balance are. */
void runSetDeviceTest(const QMap<QString, QString> &params)
{
	bool deviceIdOk = false;
	uint32_t deviceId = params.value(QStringLiteral("device_id")).toUInt(&deviceIdOk);
	if (!deviceIdOk) {
		blog(LOG_INFO, "[ptz-ui-test] set_device: missing/invalid device_id");
		return;
	}

	QModelIndex index = ptzDeviceList.indexFromDeviceId(deviceId);
	if (!index.isValid()) {
		blog(LOG_INFO, "[ptz-ui-test] set_device: device_id %u not found", deviceId);
		return;
	}

	calldata cd = {};
	if (params.contains(QStringLiteral("power_on"))) {
		bool on = params.value(QStringLiteral("power_on")).toLower() == QStringLiteral("true");
		calldata_set_bool(&cd, "power_on", on);
	}
	if (params.contains(QStringLiteral("focus_af_enabled"))) {
		bool enabled = params.value(QStringLiteral("focus_af_enabled")).toLower() == QStringLiteral("true");
		calldata_set_bool(&cd, "focus_af_enabled", enabled);
	}
	if (params.contains(QStringLiteral("wb_mode")))
		calldata_set_int(&cd, "wb_mode", params.value(QStringLiteral("wb_mode")).toLongLong());

	ptzDeviceList.callDevice(index, "ptz_set", &cd);
	calldata_free(&cd);

	blog(LOG_INFO, "[ptz-ui-test] set_device device_id=%u", deviceId);
}

} // namespace

/* Request params:
 *   device_id         - the target device's numeric id
 *   power_on          - optional, "True"/"False" (Python's str(bool))
 *   focus_af_enabled  - optional, "True"/"False" (Python's str(bool))
 *   wb_mode           - optional, integer white-balance mode
 */
void registerSetDeviceTest(PTZUITestHarness *harness)
{
	harness->registerTest(QStringLiteral("set_device"), &runSetDeviceTest);
}
