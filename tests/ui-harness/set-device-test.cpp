/* PTZ UI test harness: one-shot device actions (triggers)
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

/* Drives PTZDevice::trigger() (the "ptz_trigger" proc handler): a one-shot
 * action, by its name */
void runTriggerDeviceTest(const QMap<QString, QString> &params)
{
	QString deviceName = params.value(QStringLiteral("device"));
	bool deviceOk = !deviceName.isEmpty();
	QModelIndex index = deviceOk ? ptzUITestDeviceIndex(deviceName) : QModelIndex();
	if (!index.isValid()) {
		blog(LOG_INFO, "[ptz-ui-test] trigger_device: missing/invalid device");
		return;
	}

	calldata cd = {};
	calldata_set_string(&cd, "name", qUtf8Printable(params.value(QStringLiteral("name"))));
	ptzDeviceList->callDevice(index, "ptz_trigger", &cd);
	calldata_free(&cd);
}

} // namespace

/* trigger_device request params:
 *   device - the device, by the UUID of its filter or the name of the source it is on
 *   name      - the one-shot action, such as "camera_report"
 */
void registerSetDeviceTest(PTZUITestHarness *harness)
{
	harness->registerTest(QStringLiteral("trigger_device"), &runTriggerDeviceTest);
}
