/* PTZ UI test harness
 *
 * Copyright 2026 Grant Likely <grant.likely@secretlab.ca>
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include "ui-test-harness.hpp"
#include "ptz-websocket.hpp"
#include "ptz.h"

#include <obs-module.h>
#include <QCoreApplication>
#include <QMetaObject>

namespace {
PTZUITestHarness *g_harness = nullptr;
obs_websocket_vendor g_vendor = nullptr;
} // namespace

obs_websocket_vendor ptzUITestVendor()
{
	return g_vendor;
}

PTZUITestHarness::PTZUITestHarness(QObject *parent) : QObject(parent)
{
	registerAppearanceRowSizingTest(this);
	registerDeviceSettingsTest(this);
	registerDiscoveryTest(this);
	registerThirdPartyDeviceTest(this);
	registerVendorEventTest(this);
}

bool ptzUITestIsDevice(obs_source_t *source)
{
	proc_handler_t *ph = source ? obs_source_get_proc_handler(source) : nullptr;
	if (!ph)
		return false;
	calldata_t cd = {};
	bool usable = proc_handler_call(ph, "ptz_get_api_version", &cd) &&
		      calldata_int(&cd, "major") == PTZ_API_VERSION_MAJOR &&
		      calldata_int(&cd, "minor") >= PTZ_API_VERSION_MINOR_NEEDED;
	calldata_free(&cd);
	return usable;
}

OBSSource ptzUITestDeviceSource(const QString &device)
{
	OBSSourceAutoRelease byUuid = obs_get_source_by_uuid(qUtf8Printable(device));
	if (byUuid && ptzUITestIsDevice(byUuid))
		return byUuid.Get();
	OBSSourceAutoRelease source = obs_get_source_by_name(qUtf8Printable(device));
	if (!source)
		return nullptr;
	if (ptzUITestIsDevice(source))
		return source.Get();
	/* The first of its filters that is a device */
	OBSSource found;
	obs_source_enum_filters(
		source,
		[](obs_source_t *, obs_source_t *filter, void *data) {
			auto found = static_cast<OBSSource *>(data);
			if (!*found && ptzUITestIsDevice(filter))
				*found = filter;
		},
		&found);
	return found;
}

OBSSource ptzUITestParentSource(obs_source_t *device)
{
	calldata_t cd = {};
	proc_handler_call(obs_source_get_proc_handler(device), "ptz_get_parent_source", &cd);
	OBSSourceAutoRelease said = static_cast<obs_source_t *>(calldata_ptr(&cd, "return"));
	calldata_free(&cd);
	if (said)
		return said.Get();
	if (obs_source_get_type(device) == OBS_SOURCE_TYPE_FILTER)
		return obs_filter_get_parent(device);
	return device;
}

QStringList ptzUITestDiscoveryKinds()
{
	QStringList kinds;
	const char *id;
	for (size_t i = 0; obs_enum_input_types(i, &id); i++) {
		if (!QByteArray(id).endsWith(".discovery"))
			continue;
		OBSDataAutoRelease defaults = obs_get_source_defaults(id);
		if (!defaults || !obs_data_has_default_value(defaults, PTZ_DEFAULTS_MAJOR_KEY) ||
		    !obs_data_has_default_value(defaults, PTZ_DEFAULTS_MINOR_KEY))
			continue;
		if (obs_data_get_int(defaults, PTZ_DEFAULTS_MAJOR_KEY) == PTZ_API_VERSION_MAJOR &&
		    obs_data_get_int(defaults, PTZ_DEFAULTS_MINOR_KEY) >= PTZ_API_VERSION_MINOR_NEEDED)
			kinds << QString::fromUtf8(id);
	}
	return kinds;
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

	/* The PTZ API's binding to obs-websocket has the vendor, which can be
	 * registered only once: the harness's request is one more on it */
	g_vendor = ptz_websocket_vendor();
	if (!g_vendor) {
		blog(LOG_INFO, "[ptz-ui-test] obs-websocket not available, harness inactive");
		return;
	}
	obs_websocket_vendor_register_request(g_vendor, "ui_test_run", &PTZUITestHarness::vendorRequestCallback,
					      g_harness);
	blog(LOG_INFO, "[ptz-ui-test] harness active (obs-websocket vendor 'obs-ptz', request 'ui_test_run')");
}
