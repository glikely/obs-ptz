/* PTZ UI test harness: set up the windows scripts/update-screenshots.py
 * takes the documentation's screenshots of
 *
 * Copyright 2026 Grant Likely <grant.likely@secretlab.ca>
 *
 * SPDX-License-Identifier: GPLv2
 */
#include "ui-test-harness.hpp"

#include <obs.hpp>
#include <obs-module.h>
#include <obs-frontend-api.h>
#include <QAction>
#include <QApplication>
#include <QComboBox>
#include <QDockWidget>
#include <QListView>
#include <QRect>
#include <QTabWidget>
#include <QWidget>

#include "ptz-list-model.hpp"
#include "ptz-controls.hpp"
#include "settings.hpp"

namespace {

QModelIndex deviceIndex(const QMap<QString, QString> &params, const char *test)
{
	bool ok = false;
	uint32_t deviceId = params.value(QStringLiteral("device_id")).toUInt(&ok);
	QModelIndex index = ok ? ptzDeviceList->indexFromDeviceId(deviceId) : QModelIndex();
	if (!index.isValid())
		blog(LOG_INFO, "[ptz-ui-test] %s: missing/invalid device_id", test);
	return index;
}

QRect geometryFrom(const QMap<QString, QString> &params)
{
	return QRect(params.value(QStringLiteral("x")).toInt(), params.value(QStringLiteral("y")).toInt(),
		     params.value(QStringLiteral("w")).toInt(), params.value(QStringLiteral("h")).toInt());
}

/* Makes a preset as a user would: a new one, saved where the camera is now,
 * with a thumbnail of what the camera shows, and named */
void runAddPresetTest(const QMap<QString, QString> &params)
{
	QModelIndex index = deviceIndex(params, "add_preset");
	if (!index.isValid())
		return;

	calldata cd = {};
	calldata_set_int(&cd, "row", -1);
	ptzDeviceList->callDevice(index, "ptz_preset_new", &cd);
	long long id = calldata_int(&cd, "return");
	calldata_free(&cd);
	if (id < 0) {
		blog(LOG_INFO, "[ptz-ui-test] add_preset: no room for another preset");
		return;
	}

	calldata save = {};
	calldata_set_int(&save, "preset_id", id);
	ptzDeviceList->callDevice(index, "ptz_preset_save", &save);
	calldata_free(&save);

	calldata name = {};
	calldata_set_int(&name, "id", id);
	calldata_set_string(&name, "name", qUtf8Printable(params.value(QStringLiteral("name"))));
	ptzDeviceList->callDevice(index, "ptz_preset_set_name", &name);
	calldata_free(&name);
}

/* Floats the PTZ Controls dock at x, y, w, h, shows the device's presets as
 * a list or, with grid=true, as thumbnails, and selects the device */
void runShowDockTest(const QMap<QString, QString> &params)
{
	auto *mainWindow = static_cast<QWidget *>(obs_frontend_get_main_window());
	auto *dock = mainWindow ? mainWindow->findChild<QDockWidget *>(QStringLiteral("ptz-dock")) : nullptr;
	if (!dock) {
		blog(LOG_INFO, "[ptz-ui-test] show_dock: no dock");
		return;
	}
	if (auto *grid = dock->findChild<QAction *>(QStringLiteral("actionPresetGridView")))
		grid->setChecked(params.value(QStringLiteral("grid")).toLower() == QStringLiteral("true"));
	dock->setFloating(true);
	dock->show();
	dock->setGeometry(geometryFrom(params));
	dock->raise();
	QModelIndex index = deviceIndex(params, "show_dock");
	auto *deviceList = dock->findChild<QListView *>(QStringLiteral("deviceList"));
	if (index.isValid() && deviceList)
		deviceList->setCurrentIndex(index);
}

/* Opens the settings dialog on a device, at x, y, w, h, on its Cameras tab */
void runShowSettingsTest(const QMap<QString, QString> &params)
{
	QModelIndex index = deviceIndex(params, "show_settings");
	if (!index.isValid())
		return;
	ptz_settings_show(index);
	for (QWidget *w : QApplication::topLevelWidgets()) {
		auto *dialog = qobject_cast<PTZSettings *>(w);
		if (!dialog)
			continue;
		if (auto *tabs = dialog->findChild<QTabWidget *>())
			tabs->setCurrentIndex(1);
		dialog->setGeometry(geometryFrom(params));
		dialog->raise();
		return;
	}
	blog(LOG_INFO, "[ptz-ui-test] show_settings: no dialog");
}

} // namespace

/* add_preset request params:
 *   device_id - the device to give a preset
 *   name      - what to call it
 * show_dock: device_id - the device to select; x, y, w, h - where to put the
 *   floating dock; grid - "true" to show the presets as thumbnails
 * show_settings: device_id - the device to show; x, y, w, h - where to put
 *   the dialog */
void registerScreenshotTest(PTZUITestHarness *harness)
{
	harness->registerTest(QStringLiteral("add_preset"), &runAddPresetTest);
	harness->registerTest(QStringLiteral("show_dock"), &runShowDockTest);
	harness->registerTest(QStringLiteral("show_settings"), &runShowSettingsTest);
}
