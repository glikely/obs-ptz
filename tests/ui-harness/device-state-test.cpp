/* PTZ UI test harness: device state test
 *
 * Copyright 2026 Grant Likely <grant.likely@secretlab.ca>
 *
 * SPDX-License-Identifier: GPLv2
 */
#include "ui-test-harness.hpp"

#include <obs.hpp>
#include <obs-module.h>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSaveFile>

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

	/* The statistics are read apart from the state, but the tests that look
	 * at them ask for them with it */
	OBSDataAutoRelease statistics = obs_data_create();
	calldata statisticsCd = {};
	calldata_set_ptr(&statisticsCd, "statistics", statistics.Get());
	ptzDeviceList->callDevice(index, "ptz_get_statistics", &statisticsCd);
	calldata_free(&statisticsCd);
	obs_data_set_obj(state, "statistics", statistics);

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

	/* Every other param is a state value: "True"/"False" a bool, anything
	 * else a number (0x.. for hex) */
	OBSDataAutoRelease state = obs_data_create();
	for (auto param = params.cbegin(); param != params.cend(); ++param) {
		if (param.key() == QStringLiteral("cmd") || param.key() == QStringLiteral("device_id"))
			continue;
		QByteArray key = param.key().toUtf8();
		QString value = param.value().toLower();
		bool isNumber = false;
		long long number = value.toLongLong(&isNumber, 0);
		if (value == QStringLiteral("true") || value == QStringLiteral("false"))
			obs_data_set_bool(state, key.constData(), value == QStringLiteral("true"));
		else if (isNumber)
			obs_data_set_int(state, key.constData(), number);
	}

	calldata cd = {};
	calldata_set_ptr(&cd, "state", state.Get());
	ptzDeviceList->callDevice(index, "ptz_request_state", &cd);
	calldata_free(&cd);

	blog(LOG_INFO, "[ptz-ui-test] set_device_state device_id=%u", deviceId);
}

/* Reports the device's camera report, from the "ptz_get_camera_report"
 * proc handler, as JSON under "report", if there is one */
void runGetCameraReportTest(const QMap<QString, QString> &params)
{
	bool deviceIdOk = false;
	uint32_t deviceId = params.value(QStringLiteral("device_id")).toUInt(&deviceIdOk);
	QString filename = params.value(QStringLiteral("filename"));
	if (!deviceIdOk || filename.isEmpty()) {
		blog(LOG_INFO, "[ptz-ui-test] get_camera_report: missing/invalid device_id or filename");
		return;
	}

	QModelIndex index = ptzDeviceList->indexFromDeviceId(deviceId);
	if (!index.isValid()) {
		blog(LOG_INFO, "[ptz-ui-test] get_camera_report: device_id %u not found", deviceId);
		return;
	}

	calldata cd = {};
	ptzDeviceList->callDevice(index, "ptz_get_camera_report", &cd);
	const char *text = calldata_string(&cd, "report");
	/* As it is: obs_data can't hold its arrays of strings */
	QJsonObject result;
	if (text && *text)
		result["report"] = QJsonDocument::fromJson(text).object();
	calldata_free(&cd);

	QSaveFile file(filename);
	if (!file.open(QIODevice::WriteOnly) || file.write(QJsonDocument(result).toJson()) < 0 || !file.commit())
		blog(LOG_INFO, "[ptz-ui-test] get_camera_report: failed to write %s", qUtf8Printable(filename));
}

} // namespace

/* get_camera_report request params:
 *   device_id - the target device's numeric id
 *   filename  - where to write the {"report": {...}} JSON result, {} if
 *               there is no report
 *
 * get_device_state request params:
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
	harness->registerTest(QStringLiteral("get_camera_report"), &runGetCameraReportTest);
}
