/* PTZ UI test harness
 *
 * Copyright 2026 Grant Likely <grant.likely@secretlab.ca>
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * Drives integration tests against a live, running OBS process from
 * inside it: a plugin of its own, obs-ptz-ui-harness, which needs nothing
 * of obs-ptz's but ptz.h, and finds devices as any controller of the PTZ
 * API does. See README.md for why this has to run inside OBS rather than
 * as an external test binary, how commands reach it (obs-websocket, on its
 * own vendor), and how to add a new test.
 */
#pragma once

#include <QObject>
#include <QMap>
#include <QString>
#include <QStringList>
#include <functional>

#include <obs.hpp>

#include "obs-websocket-api.h"

class PTZUITestHarness : public QObject {
	Q_OBJECT

public:
	using TestFn = std::function<void(const QMap<QString, QString> &params)>;

	explicit PTZUITestHarness(QObject *parent = nullptr);

	/* Registers a test function under `name`, matching a request's
	 * cmd= value. Call from each test's own register*Test(
	 * PTZUITestHarness *) function - see appearance-row-sizing-test.hpp
	 * for the pattern - not directly. */
	void registerTest(const QString &name, TestFn fn);

	/* Registers a test function that is run on obs-websocket's own thread, in
	 * the request, before it is answered, rather than queued for the main thread:
	 * for one that touches no Qt object, and whose effect has to come before the
	 * answer, as an event it emits does */
	void registerInlineTest(const QString &name, TestFn fn);

	/* Runs the registered test named `cmd` with `params`. Only call
	 * already on the Qt GUI thread - see vendorRequestCallback()'s own
	 * comment in the .cpp for why that matters. */
	void dispatch(const QString &cmd, const QMap<QString, QString> &params);

	/* obs_websocket_request_callback_function - passed to
	 * obs_websocket_vendor_register_request() by obs_module_post_load()
	 * (a free function, hence public rather than a friend). Not meant
	 * to be called any other way. */
	static void vendorRequestCallback(obs_data_t *request_data, obs_data_t *response_data, void *priv_data);

private:
	QMap<QString, TestFn> tests;
	QMap<QString, TestFn> inlineTests;
};

/* Whether a source is a device of the PTZ API, as a controller finds one: its
 * proc_handler answers ptz_get_api_version with a version it can use (see
 * docs/ptz-device-api.md) */
bool ptzUITestIsDevice(obs_source_t *source);

/* The device a test names, as the "device" param of its request: its UUID, or
 * the name of its source (a device that is a source), or of the source its
 * filter is on. Null if there is none. Found as any controller finds one, from
 * libobs and the PTZ API, not from this plugin's own list. */
OBSSource ptzUITestDeviceSource(const QString &device);

/* The source a device shows the video of: what its ptz_get_parent_source says,
 * or, if it doesn't say, its filter's parent, or itself if it is a source */
OBSSource ptzUITestParentSource(obs_source_t *device);

/* The ids of the discovery kinds (see "Discovery kinds" in docs/ptz-device-api.md):
 * the hidden input kinds with ids ending in ".discovery" whose defaults carry the
 * PTZ API version marker with a version the plugin can use */
QStringList ptzUITestDiscoveryKinds();

/* The harness's obs-websocket vendor, "obs-ptz-ui-harness", for a test that
 * emits an event as it. Null if there is no obs-websocket. */
obs_websocket_vendor ptzUITestVendor();

/* Test registration hooks */
void registerAppearanceRowSizingTest(PTZUITestHarness *harness);
void registerDeviceSettingsTest(PTZUITestHarness *harness);
void registerDiscoveryTest(PTZUITestHarness *harness);
void registerThirdPartyDeviceTest(PTZUITestHarness *harness);
void registerVendorEventTest(PTZUITestHarness *harness);
