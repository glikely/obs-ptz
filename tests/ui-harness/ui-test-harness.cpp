/* PTZ UI test harness
 *
 * Copyright 2026 Grant Likely <grant.likely@secretlab.ca>
 *
 * SPDX-License-Identifier: GPLv2
 */
#include "ui-test-harness.hpp"

#include <obs-module.h>
#include <QCoreApplication>
#include <QMetaObject>

namespace {
PTZUITestHarness *g_harness = nullptr;
}

PTZUITestHarness::PTZUITestHarness(QObject *parent) : QObject(parent)
{
	registerAppearanceRowSizingTest(this);
	registerPresetExportImportTest(this);
	registerDeviceStatusTest(this);
	registerMoveDeviceTest(this);
	registerSetDeviceTest(this);
	registerUpdateDeviceTest(this);
}

void PTZUITestHarness::registerTest(const QString &name, TestFn fn)
{
	tests[name] = std::move(fn);
}

void PTZUITestHarness::dispatch(const QString &cmd, const QMap<QString, QString> &params)
{
	auto it = tests.constFind(cmd);
	if (it == tests.constEnd()) {
		blog(LOG_INFO, "[ptz-ui-test] unknown cmd '%s', ignoring", qUtf8Printable(cmd));
		return;
	}
	blog(LOG_INFO, "[ptz-ui-test] dispatching cmd '%s'", qUtf8Printable(cmd));
	(*it)(params);
}

void PTZUITestHarness::vendorRequestCallback(obs_data_t *request_data, obs_data_t *response_data, void *priv_data)
{
	/* obs-websocket runs CallVendorRequest (and every other request)
	 * on its own QThreadPool (websocketserver/WebSocketServer.cpp),
	 * never the Qt GUI thread. Every test here drives real widgets
	 * (findChild, ->trigger(), ...), which is only safe on the GUI
	 * thread, so this callback itself must not touch any of that - it
	 * only parses the request and posts the actual work over via
	 * qApp, then acknowledges receipt immediately. The test's own
	 * result still only reaches the caller via its blog()
	 * ("[ptz-ui-test] ..." lines), same as before; this only changes
	 * how the *request* arrives, not how results are reported. */
	auto *harness = static_cast<PTZUITestHarness *>(priv_data);

	QString cmd = QString::fromUtf8(obs_data_get_string(request_data, "cmd"));
	QMap<QString, QString> params;
	for (obs_data_item_t *item = obs_data_first(request_data); item; obs_data_item_next(&item))
		params[QString::fromUtf8(obs_data_item_get_name(item))] =
			QString::fromUtf8(obs_data_item_get_string(item));

	if (cmd.isEmpty()) {
		obs_data_set_bool(response_data, "accepted", false);
		obs_data_set_string(response_data, "error", "missing 'cmd' field");
		return;
	}

	QMetaObject::invokeMethod(
		qApp, [harness, cmd, params]() { harness->dispatch(cmd, params); }, Qt::QueuedConnection);

	obs_data_set_bool(response_data, "accepted", true);
}

void ptz_load_ui_tests(void)
{
	if (!qEnvironmentVariableIsSet("PTZ_UI_TEST_HARNESS"))
		return;

	/* Intentionally never deleted: lives for the plugin's process
	 * lifetime, same as PTZControls itself (ptz-controls.cpp). */
	g_harness = new PTZUITestHarness();

	obs_websocket_vendor vendor = obs_websocket_register_vendor("obs-ptz");
	if (!vendor) {
		blog(LOG_INFO, "[ptz-ui-test] obs-websocket not available, harness inactive");
		return;
	}
	obs_websocket_vendor_register_request(vendor, "ui_test_run", &PTZUITestHarness::vendorRequestCallback,
					      g_harness);
	blog(LOG_INFO, "[ptz-ui-test] harness active (obs-websocket vendor 'obs-ptz', request 'ui_test_run')");
}
