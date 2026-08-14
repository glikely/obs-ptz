/* PTZ UI test harness: which of the dock's controls are enabled
 *
 * Copyright 2026 Grant Likely <grant.likely@secretlab.ca>
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "ui-test-harness.hpp"

#include <obs.hpp>
#include <obs-module.h>
#include <obs-frontend-api.h>
#include <cstring>
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
		auto index = ptzUITestDeviceIndex(params.value(QStringLiteral("device")));
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

struct HotkeyFind {
	const char *name;
	obs_hotkey_id id = OBS_INVALID_HOTKEY_ID;
	QString description;
};

bool findHotkey(void *data, obs_hotkey_id id, obs_hotkey_t *key)
{
	auto *find = static_cast<HotkeyFind *>(data);
	if (strcmp(obs_hotkey_get_name(key), find->name) != 0)
		return true;
	find->id = id;
	find->description = QString::fromUtf8(obs_hotkey_get_description(key));
	return false;
}

/* Presses a hotkey of the dock, as a key bound to it does */
void runFireHotkeyTest(const QMap<QString, QString> &params)
{
	QByteArray name = params.value(QStringLiteral("name")).toUtf8();
	HotkeyFind find{name.constData()};
	obs_enum_hotkeys(findHotkey, &find);
	if (find.id == OBS_INVALID_HOTKEY_ID) {
		blog(LOG_INFO, "[ptz-ui-test] fire_hotkey: no hotkey %s", name.constData());
		return;
	}
	obs_hotkey_trigger_routed_callback(find.id, true);
	obs_hotkey_trigger_routed_callback(find.id, false);
	blog(LOG_INFO, "[ptz-ui-test] fire_hotkey: %s", name.constData());
}

/* Writes {"found", "description"} of a hotkey, to `filename` */
void runGetHotkeyTest(const QMap<QString, QString> &params)
{
	QByteArray name = params.value(QStringLiteral("name")).toUtf8();
	HotkeyFind find{name.constData()};
	obs_enum_hotkeys(findHotkey, &find);
	OBSDataAutoRelease result = obs_data_create();
	obs_data_set_bool(result, "found", find.id != OBS_INVALID_HOTKEY_ID);
	obs_data_set_string(result, "description", qUtf8Printable(find.description));
	obs_data_save_json_safe(result, qUtf8Printable(params.value(QStringLiteral("filename"))), "tmp", "bak");
}

} // namespace

/* Request params:
 *   device - the device, by the UUID of its filter or the name of the source it is on
 *   filename  - where to write the {"found", "selected", "enabled"} JSON
 *               result
 */
void registerDockControlsTest(PTZUITestHarness *harness)
{
	harness->registerTest(QStringLiteral("get_dock_controls"), &runDockControlsTest);
	harness->registerTest(QStringLiteral("fire_hotkey"), &runFireHotkeyTest);
	harness->registerTest(QStringLiteral("get_hotkey"), &runGetHotkeyTest);
}

/* fire_hotkey request params:
 *   name - the hotkey's name, such as "PTZ.Recall2"
 * get_hotkey: name as above, and filename - where to write the {"found",
 *   "description"} JSON result
 */
