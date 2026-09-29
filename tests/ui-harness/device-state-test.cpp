/* PTZ UI test harness: device state test
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

/* Reports a device's whole transient state through the "ptz_get_state"
 * proc handler (PTZDevice::get_state(), which is saveState() plus a few
 * more fields for PTZListModel's row display -- see src/ptz-device.cpp)
 * as JSON under "state". */
void runGetDeviceStateTest(const QMap<QString, QString> &params)
{
	bool deviceIdOk = false;
	uint32_t deviceId = params.value(QStringLiteral("device_id")).toUInt(&deviceIdOk);
	QString filename = params.value(QStringLiteral("filename"));
	if (!deviceIdOk || filename.isEmpty()) {
		blog(LOG_INFO, "[ptz-ui-test] get_device_state: missing/invalid device_id or filename");
		return;
	}

	QModelIndex index = ptzDeviceList->indexFromDeviceId(deviceId);
	if (!index.isValid()) {
		blog(LOG_INFO, "[ptz-ui-test] get_device_state: device_id %u not found", deviceId);
		return;
	}

	OBSDataAutoRelease state = obs_data_create();
	calldata cd = {};
	calldata_set_ptr(&cd, "state", state.Get());
	ptzDeviceList->callDevice(index, "ptz_get_state", &cd);
	calldata_free(&cd);

	OBSDataAutoRelease result = obs_data_create();
	obs_data_set_obj(result, "state", state);
	if (!obs_data_save_json_safe(result, qUtf8Printable(filename), "tmp", "bak"))
		blog(LOG_INFO, "[ptz-ui-test] get_device_state: failed to write %s", qUtf8Printable(filename));
}

/* Drives the "ptz_request_state" proc handler (PTZDevice::request_state(),
 * see src/ptz-device.cpp) with a state object holding just the values given,
 * as the settings dialog's state view does. */
void runSetDeviceStateTest(const QMap<QString, QString> &params)
{
	bool deviceIdOk = false;
	uint32_t deviceId = params.value(QStringLiteral("device_id")).toUInt(&deviceIdOk);
	if (!deviceIdOk) {
		blog(LOG_INFO, "[ptz-ui-test] set_device_state: missing/invalid device_id");
		return;
	}

	QModelIndex index = ptzDeviceList->indexFromDeviceId(deviceId);
	if (!index.isValid()) {
		blog(LOG_INFO, "[ptz-ui-test] set_device_state: device_id %u not found", deviceId);
		return;
	}

	OBSDataAutoRelease state = obs_data_create();
	if (params.contains(QStringLiteral("power_on")))
		obs_data_set_bool(state, "power_on", params.value(QStringLiteral("power_on")).toLower() == "true");
	if (params.contains(QStringLiteral("focus_af_enabled")))
		obs_data_set_bool(state, "focus_af_enabled",
				  params.value(QStringLiteral("focus_af_enabled")).toLower() == "true");
	if (params.contains(QStringLiteral("tally_on")))
		obs_data_set_bool(state, "tally_on", params.value(QStringLiteral("tally_on")).toLower() == "true");
	if (params.contains(QStringLiteral("tally_preview")))
		obs_data_set_bool(state, "tally_preview",
				  params.value(QStringLiteral("tally_preview")).toLower() == "true");
	if (params.contains(QStringLiteral("wb_mode")))
		obs_data_set_int(state, "wb_mode", params.value(QStringLiteral("wb_mode")).toLongLong());

	calldata cd = {};
	calldata_set_ptr(&cd, "state", state.Get());
	ptzDeviceList->callDevice(index, "ptz_request_state", &cd);
	calldata_free(&cd);

	blog(LOG_INFO, "[ptz-ui-test] set_device_state device_id=%u", deviceId);
}

/* Hands one device the OBS frontend event it gets when OBS has finished
 * loading, or is closing (see PTZDevice::onFrontendEvent()). OBS does that
 * once, at startup and when it quits, so a test can't wait for it. */
void runObsEventTest(const QMap<QString, QString> &params)
{
	bool deviceIdOk = false;
	uint32_t deviceId = params.value(QStringLiteral("device_id")).toUInt(&deviceIdOk);
	QString event = params.value(QStringLiteral("event"));
	if (!deviceIdOk || (event != QStringLiteral("startup") && event != QStringLiteral("shutdown"))) {
		blog(LOG_INFO, "[ptz-ui-test] obs_event: missing/invalid device_id or event");
		return;
	}

	bool found = PTZDevice::deliverFrontendEvent(deviceId, event == QStringLiteral("startup")
								       ? OBS_FRONTEND_EVENT_FINISHED_LOADING
								       : OBS_FRONTEND_EVENT_SCRIPTING_SHUTDOWN);

	blog(LOG_INFO, "[ptz-ui-test] obs_event device_id=%u event=%s%s", deviceId, qUtf8Printable(event),
	     found ? "" : " (no such device)");
}

} // namespace

/* get_device_state request params:
 *   device_id - the target device's numeric id
 *   filename  - where to write the {"state": {...}} JSON result
 *
 * set_device_state request params:
 *   device_id         - the target device's numeric id
 *   power_on          - optional, "True"/"False"
 *   focus_af_enabled  - optional, "True"/"False"
 *   tally_on          - optional, "True"/"False"
 *   tally_preview     - optional, "True"/"False"
 *   wb_mode           - optional, integer white-balance mode
 * A request with none of the optional params asks for nothing.
 *
 * obs_event request params:
 *   device_id - the target device's numeric id
 *   event     - "startup" or "shutdown"
 */
void registerDeviceStateTest(PTZUITestHarness *harness)
{
	harness->registerTest(QStringLiteral("get_device_state"), &runGetDeviceStateTest);
	harness->registerTest(QStringLiteral("set_device_state"), &runSetDeviceStateTest);
	harness->registerTest(QStringLiteral("obs_event"), &runObsEventTest);
}
