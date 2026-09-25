/* PTZ UI test harness
 *
 * Copyright 2026 Grant Likely <grant.likely@secretlab.ca>
 *
 * SPDX-License-Identifier: GPLv2
 *
 * Drives UI-level integration tests against a live, running OBS
 * process from inside the plugin itself - see README.md for why this
 * has to run in-process rather than as an external test binary, how
 * commands reach it (obs-websocket, not a polled file - also in
 * README.md), and how to add a new test.
 */
#pragma once

#include <QObject>
#include <QMap>
#include <QString>
#include <functional>

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

	/* Runs the registered test named `cmd` with `params`. Only call
	 * already on the Qt GUI thread - see vendorRequestCallback()'s own
	 * comment in the .cpp for why that matters. */
	void dispatch(const QString &cmd, const QMap<QString, QString> &params);

	/* obs_websocket_request_callback_function - passed to
	 * obs_websocket_vendor_register_request() by ptz_load_ui_tests()
	 * (a free function, hence public rather than a friend). Not meant
	 * to be called any other way. */
	static void vendorRequestCallback(obs_data_t *request_data, obs_data_t *response_data, void *priv_data);

private:
	QMap<QString, TestFn> tests;
};

/* Registers this plugin as an obs-websocket vendor ("obs-ptz", request
 * type "ui_test_run") if PTZ_UI_TEST_HARNESS=1 is set in the
 * environment; otherwise a no-op (including when obs-websocket itself
 * isn't loaded/running - obs_websocket_register_vendor() degrades
 * gracefully). Declared here but only ever called - and only ever
 * linked in at all - when the ENABLE_UI_TESTS CMake option is on.
 *
 * Must run from obs_module_post_load(), not obs_module_load():
 * obs-websocket-api.h's obs_websocket_register_vendor() requires it,
 * since module load order between plugins isn't otherwise guaranteed
 * and obs-websocket needs to have already finished its own
 * obs_module_load() first. See ptz.c. */
extern "C" void ptz_load_ui_tests(void);

/* Test registration hooks */
void registerAppearanceRowSizingTest(PTZUITestHarness *harness);
void registerPresetExportImportTest(PTZUITestHarness *harness);
void registerDeviceStatusTest(PTZUITestHarness *harness);
void registerMoveDeviceTest(PTZUITestHarness *harness);
void registerSetDeviceTest(PTZUITestHarness *harness);
void registerUpdateDeviceTest(PTZUITestHarness *harness);
void registerPresetViewTest(PTZUITestHarness *harness);
