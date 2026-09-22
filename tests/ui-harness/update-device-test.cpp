/* PTZ UI test harness: full device settings update test
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

/* Drives PTZListModel::update(index, settings) with the device's own
 * current settings (from PTZListModel::save(), see PTZDevice::save()/
 * PTZVisca::save() in src/ptz-device.cpp/src/ptz-visca.cpp) overlaid with
 * whatever fields the caller provided -- the same thing
 * PTZSettings::on_applyButton_clicked()/updateProperties() do with
 * propertiesView->GetSettings() when the user edits the properties
 * dialog and clicks Apply/OK (src/settings.cpp). In particular this is
 * how the "Protocol" list (PTZVisca::get_obs_properties(), see
 * src/ptz-visca.cpp) switches a live VISCA device between the
 * serial/UDP/TCP transports: set "type" to "visca"/"visca-over-ip"/
 * "visca-over-tcp" plus that transport's "host"/"port"/"address" fields,
 * reusing the same field names the properties dialog does (see
 * PTZVisca::update()'s "type" -> ViscaTransport switch and
 * ViscaUDPTransport::update()/ViscaTCPTransport::update()). Unlike
 * set_device/move_device (which go through narrow calldata-based proc
 * handlers for individual properties), this always seeds from the
 * device's current full settings first so fields the caller doesn't
 * override keep their existing values instead of resetting to defaults,
 * matching how the properties dialog itself is always pre-populated
 * before the user starts editing. */
void runUpdateDeviceTest(const QMap<QString, QString> &params)
{
	bool deviceIdOk = false;
	uint32_t deviceId = params.value(QStringLiteral("device_id")).toUInt(&deviceIdOk);
	if (!deviceIdOk) {
		blog(LOG_INFO, "[ptz-ui-test] update_device: missing/invalid device_id");
		return;
	}

	QModelIndex index = ptzDeviceList.indexFromDeviceId(deviceId);
	if (!index.isValid()) {
		blog(LOG_INFO, "[ptz-ui-test] update_device: device_id %u not found", deviceId);
		return;
	}

	OBSData cfg = obs_data_create();
	obs_data_release(cfg);
	ptzDeviceList.save(index, cfg);

	if (params.contains(QStringLiteral("type")))
		obs_data_set_string(cfg, "type", qUtf8Printable(params.value(QStringLiteral("type"))));
	if (params.contains(QStringLiteral("host")))
		obs_data_set_string(cfg, "host", qUtf8Printable(params.value(QStringLiteral("host"))));
	if (params.contains(QStringLiteral("port"))) {
		/* Numeric for VISCA UDP/TCP, but a serial device path (a
		 * string, e.g. "/dev/ttyUSB0") when switching to "visca". */
		QString portParam = params.value(QStringLiteral("port"));
		bool portOk = false;
		int port = portParam.toInt(&portOk);
		if (portOk)
			obs_data_set_int(cfg, "port", port);
		else
			obs_data_set_string(cfg, "port", qUtf8Printable(portParam));
	}
	if (params.contains(QStringLiteral("address"))) {
		bool addressOk = false;
		int address = params.value(QStringLiteral("address")).toInt(&addressOk);
		if (addressOk)
			obs_data_set_int(cfg, "address", address);
	}

	ptzDeviceList.update(index, cfg);

	blog(LOG_INFO, "[ptz-ui-test] update_device device_id=%u", deviceId);
}

} // namespace

/* Request params:
 *   device_id      - the target device's numeric id
 *   type           - optional, new device "type" (e.g. "visca",
 *                    "visca-over-ip", "visca-over-tcp", "pelco")
 *   host           - optional, new "host" (VISCA UDP/TCP)
 *   port           - optional, new "port" (VISCA UDP/TCP numeric port,
 *                    or serial device path when type is "visca")
 *   address        - optional, new "address" (VISCA bus/camera address)
 * Fields left out keep the device's current value rather than resetting.
 */
void registerUpdateDeviceTest(PTZUITestHarness *harness)
{
	harness->registerTest(QStringLiteral("update_device"), &runUpdateDeviceTest);
}
