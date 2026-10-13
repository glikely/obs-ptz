/* PTZ UI test harness: device state test
 *
 * Copyright 2026 Grant Likely <grant.likely@secretlab.ca>
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include "ui-test-harness.hpp"

#include <obs.hpp>
#include <obs-module.h>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSaveFile>

#include "ptz-list-model.hpp"

namespace {

/* Drives the "ptz_request_state" proc handler (PTZDevice::request_state(),
 * see src/core/ptz-device.cpp) with a state object holding just the values given,
 * as the settings dialog's state view does. */
void runSetDeviceStateTest(const QMap<QString, QString> &params)
{
	QString deviceName = params.value(QStringLiteral("device"));
	bool deviceOk = !deviceName.isEmpty();
	if (!deviceOk) {
		blog(LOG_INFO, "[ptz-ui-test] set_device_state: missing/invalid device");
		return;
	}

	QModelIndex index = ptzUITestDeviceIndex(deviceName);
	if (!index.isValid()) {
		blog(LOG_INFO, "[ptz-ui-test] set_device_state: device %s not found", qUtf8Printable(deviceName));
		return;
	}

	/* Every other param is a state value: "True"/"False" a bool, anything
	 * else a number (0x.. for hex) */
	OBSDataAutoRelease state = obs_data_create();
	for (auto param = params.cbegin(); param != params.cend(); ++param) {
		if (param.key() == QStringLiteral("cmd") || param.key() == QStringLiteral("device"))
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

	blog(LOG_INFO, "[ptz-ui-test] set_device_state device=%s", qUtf8Printable(deviceName));
}

/* Reports the device's camera report, from the "ptz_get_camera_report"
 * proc handler, as JSON under "report", if there is one */
void runGetCameraReportTest(const QMap<QString, QString> &params)
{
	QString deviceName = params.value(QStringLiteral("device"));
	bool deviceOk = !deviceName.isEmpty();
	QString filename = params.value(QStringLiteral("filename"));
	if (!deviceOk || filename.isEmpty()) {
		blog(LOG_INFO, "[ptz-ui-test] get_camera_report: missing/invalid device or filename");
		return;
	}

	QModelIndex index = ptzUITestDeviceIndex(deviceName);
	if (!index.isValid()) {
		blog(LOG_INFO, "[ptz-ui-test] get_camera_report: device %s not found", qUtf8Printable(deviceName));
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
 *   device - the device, by the UUID of its filter or the name of the source it is on
 *   filename  - where to write the {"report": {...}} JSON result, {} if
 *               there is no report
 *
 * set_device_state request params:
 *   device         - the device, by the UUID of its filter or the name of the source it is on
 *   power_on          - optional, "True"/"False"
 *   focus_af_enabled  - optional, "True"/"False"
 *   tally_on          - optional, "True"/"False"
 *   tally_preview     - optional, "True"/"False"
 *   wb_mode           - optional, integer white-balance mode
 * A request with none of the optional params asks for nothing.
 *
 * obs_event request params:
 *   device - the device, by the UUID of its filter or the name of the source it is on
 *   event     - "startup" or "shutdown"
 */
void registerDeviceStateTest(PTZUITestHarness *harness)
{
	harness->registerTest(QStringLiteral("set_device_state"), &runSetDeviceStateTest);
	harness->registerTest(QStringLiteral("get_camera_report"), &runGetCameraReportTest);
}
