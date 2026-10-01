/* PTZ UI test harness: which of the dock's controls are enabled
 *
 * Copyright 2026 Grant Likely <grant.likely@secretlab.ca>
 *
 * SPDX-License-Identifier: GPLv2
 */

#include "ui-test-harness.hpp"

#include <obs.hpp>
#include <obs-module.h>
#include <obs-frontend-api.h>
#include <QListView>
#include <QWidget>

#include "ptz-list-model.hpp"

namespace {

/* The dock's controls whose enabled state follows what the camera can do */
const char *const dockControls[] = {
	"panTiltButton_up", "panTiltButton_home", "panTiltTouch",    "zoomButton_tele",      "zoomButton_wide",
	"focusButton_auto", "focusButton_near",   "focusButton_far", "focusButton_onetouch", "presetListView",
};

/* Selects a device in the PTZ Controls dock's camera list, then reports,
 * as JSON:
 *
 *   found    - whether the dock's camera list was found
 *   selected - whether the device is selected
 *   enabled  - an object with whether each of dockControls is enabled */
void runDockControlsTest(const QMap<QString, QString> &params)
{
	QString filename = params.value(QStringLiteral("filename"));
	if (filename.isEmpty()) {
		blog(LOG_INFO, "[ptz-ui-test] get_dock_controls: missing filename");
		return;
	}
	auto *mainWindow = static_cast<QWidget *>(obs_frontend_get_main_window());
	auto *deviceList = mainWindow ? mainWindow->findChild<QListView *>(QStringLiteral("deviceList")) : nullptr;
	OBSDataAutoRelease result = obs_data_create();
	obs_data_set_bool(result, "found", deviceList);
	if (deviceList) {
		auto index = ptzDeviceList->indexFromDeviceId(params.value(QStringLiteral("device_id")).toUInt());
		deviceList->setCurrentIndex(index);
		obs_data_set_bool(result, "selected", index.isValid() && deviceList->currentIndex() == index);
		OBSDataAutoRelease enabled = obs_data_create();
		for (const char *name : dockControls) {
			auto *widget = mainWindow->findChild<QWidget *>(QString::fromLatin1(name));
			if (widget)
				obs_data_set_bool(enabled, name, widget->isEnabled());
		}
		obs_data_set_obj(result, "enabled", enabled);
	}
	if (!obs_data_save_json_safe(result, qUtf8Printable(filename), "tmp", "bak"))
		blog(LOG_INFO, "[ptz-ui-test] get_dock_controls: failed to write %s", qUtf8Printable(filename));
}

} // namespace

/* Request params:
 *   device_id - the device to select in the camera list
 *   filename  - where to write the {"found", "selected", "enabled"} JSON
 *               result
 */
void registerDockControlsTest(PTZUITestHarness *harness)
{
	harness->registerTest(QStringLiteral("get_dock_controls"), &runDockControlsTest);
}
