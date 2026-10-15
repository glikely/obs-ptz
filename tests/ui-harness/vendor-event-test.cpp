/* PTZ UI test harness: an event of the "obs-ptz" obs-websocket vendor, and a
 * main thread kept busy
 *
 * Copyright 2026 Grant Likely <grant.likely@secretlab.ca>
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include "ui-test-harness.hpp"

#include <obs.hpp>
#include <obs-module.h>
#include <QThread>

namespace {

/* Emits an event as the vendor, for a test of what a client is sent */
void runEmitVendorEventTest(const QMap<QString, QString> &params)
{
	QString type = params.value(QStringLiteral("type"));
	if (type.isEmpty()) {
		blog(LOG_INFO, "[ptz-ui-test] emit_vendor_event: missing type");
		return;
	}
	OBSDataAutoRelease data = obs_data_create_from_json(qUtf8Printable(params.value(QStringLiteral("data"), "{}")));
	if (!obs_websocket_vendor_emit_event(ptzUITestVendor(), qUtf8Printable(type), data))
		blog(LOG_INFO, "[ptz-ui-test] emit_vendor_event: could not emit %s", qUtf8Printable(type));
}

/* Keeps the main thread busy, as a slow frame or a modal dialog does, for a
 * test of what waits for it */
void runHoldMainThreadTest(const QMap<QString, QString> &params)
{
	QThread::msleep(params.value(QStringLiteral("ms")).toULong());
}

} // namespace

/* emit_vendor_event request params:
 *   type - the event's name
 *   data - its data, as a JSON object; {} if left out
 *
 * hold_main_thread request params:
 *   ms - how long to sleep in the main thread
 */
void registerVendorEventTest(PTZUITestHarness *harness)
{
	harness->registerTest(QStringLiteral("emit_vendor_event"), &runEmitVendorEventTest);
	harness->registerTest(QStringLiteral("hold_main_thread"), &runHoldMainThreadTest);
}
