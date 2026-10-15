/* PTZ UI test harness: preset list view test
 *
 * Copyright 2026 Grant Likely <grant.likely@secretlab.ca>
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include "ui-test-harness.hpp"

#include <obs.hpp>
#include <obs-module.h>
#include <obs-frontend-api.h>
#include <QAction>
#include <QApplication>
#include <QKeyEvent>
#include <QLineEdit>
#include <QListView>
#include <QMenu>
#include <QToolButton>
#include <QWidget>

#include "ptz-list-model.hpp"

namespace {

/* Optionally selects a device in the PTZ Controls dock's camera list, then
 * reports what the dock's preset list is showing, as JSON:
 *
 *   found          - whether the dock's two lists were found
 *   rows           - the text of each row the preset list shows
 *   device_names   - the names of all the devices, as the camera list
 *                    shows them. The preset list has no business showing
 *                    any of these: a QListView rooted at an invalid index
 *                    shows the top level of its model, and for the device
 *                    model that is the list of cameras
 *   selected       - whether a device is selected in the camera list
 *   add_menu       - the entries of the menu on the Add button, [{"text",
 *                    "checked"}]: none unless the device has both stores
 *   add_stores     - the stores the device can make a preset in, ["camera",
 *                    "local"] (only those it has) */
void runPresetViewTest(const QMap<QString, QString> &params)
{
	QString filename = params.value(QStringLiteral("filename"));
	if (filename.isEmpty()) {
		blog(LOG_INFO, "[ptz-ui-test] get_preset_view: missing filename");
		return;
	}

	auto *mainWindow = static_cast<QWidget *>(obs_frontend_get_main_window());
	auto *deviceList = mainWindow ? mainWindow->findChild<QListView *>(QStringLiteral("deviceList")) : nullptr;
	auto *presetList = mainWindow ? mainWindow->findChild<QListView *>(QStringLiteral("presetListView")) : nullptr;

	OBSDataAutoRelease result = obs_data_create();
	obs_data_set_bool(result, "found", deviceList && presetList);

	if (deviceList && presetList) {
		QString select = params.value(QStringLiteral("select"));
		if (select == QStringLiteral("none")) {
			deviceList->selectionModel()->setCurrentIndex(QModelIndex(), QItemSelectionModel::Clear);
		} else if (!select.isEmpty()) {
			deviceList->setCurrentIndex(ptzUITestDeviceIndex(select));
		}

		/* The Add button, by the action it is the button of */
		QToolButton *addButton = nullptr;
		for (auto *button : mainWindow->findChildren<QToolButton *>()) {
			if (button->defaultAction() &&
			    button->defaultAction()->objectName() == QStringLiteral("actionPresetAdd"))
				addButton = button;
		}
		QString add = params.value(QStringLiteral("add"));
		if (!add.isEmpty()) {
			/* "button" is a click on it, "camera" and "local" are the menu's
			 * entries, and "toggle_default" the menu's setting of what Add does. The
			 * name it opens to edit is left as it is. */
			QAction *action = nullptr;
			if (add == QStringLiteral("camera"))
				action = mainWindow->findChild<QAction *>(QStringLiteral("actionPresetAddCamera"));
			else if (add == QStringLiteral("local"))
				action = mainWindow->findChild<QAction *>(QStringLiteral("actionPresetAddLocal"));
			else if (add == QStringLiteral("toggle_default"))
				action = mainWindow->findChild<QAction *>(QStringLiteral("actionPresetDefaultCamera"));
			if (action)
				action->trigger();
			else if (addButton && add == QStringLiteral("button"))
				addButton->click();
			if (auto *editor = presetList->findChild<QLineEdit *>()) {
				QKeyEvent escape(QEvent::KeyPress, Qt::Key_Escape, Qt::NoModifier);
				QApplication::sendEvent(editor, &escape);
			}
		}

		OBSDataArrayAutoRelease rows = obs_data_array_create();
		auto *model = presetList->model();
		auto root = presetList->rootIndex();
		for (int row = 0; model && row < model->rowCount(root); row++) {
			OBSDataAutoRelease item = obs_data_create();
			auto index = model->index(row, 0, root);
			obs_data_set_string(item, "text", qUtf8Printable(index.data(Qt::DisplayRole).toString()));
			obs_data_set_string(item, "id", qUtf8Printable(index.data(Qt::UserRole).toString()));
			obs_data_set_string(item, "store",
					    qUtf8Printable(index.data(PTZListModel::PresetStoreRole).toString()));
			obs_data_set_string(item, "tooltip", qUtf8Printable(index.data(Qt::ToolTipRole).toString()));
			obs_data_array_push_back(rows, item);
		}
		obs_data_set_array(result, "rows", rows);

		OBSDataArrayAutoRelease menu = obs_data_array_create();
		if (addButton && addButton->menu()) {
			for (auto *action : addButton->menu()->actions()) {
				if (action->isSeparator())
					continue;
				OBSDataAutoRelease item = obs_data_create();
				obs_data_set_string(item, "text", qUtf8Printable(action->text()));
				obs_data_set_bool(item, "checked", action->isChecked());
				obs_data_array_push_back(menu, item);
			}
		}
		obs_data_set_array(result, "add_menu", menu);
		OBSDataArrayAutoRelease stores = obs_data_array_create();
		for (const QString &store : ptzDeviceList->presetStores(deviceList->currentIndex())) {
			OBSDataAutoRelease item = obs_data_create();
			obs_data_set_string(item, "text", qUtf8Printable(store));
			obs_data_array_push_back(stores, item);
		}
		obs_data_set_array(result, "add_stores", stores);

		OBSDataArrayAutoRelease names = obs_data_array_create();
		for (int row = 0; row < ptzDeviceList->rowCount(); row++) {
			OBSDataAutoRelease item = obs_data_create();
			obs_data_set_string(
				item, "text",
				qUtf8Printable(ptzDeviceList->index(row, 0).data(Qt::DisplayRole).toString()));
			obs_data_array_push_back(names, item);
		}
		obs_data_set_array(result, "device_names", names);
		obs_data_set_bool(result, "selected", deviceList->currentIndex().isValid());
	}

	if (!obs_data_save_json_safe(result, qUtf8Printable(filename), "tmp", "bak"))
		blog(LOG_INFO, "[ptz-ui-test] get_preset_view: failed to write %s", qUtf8Printable(filename));
}

/* Has OBS save, which has the dock write its config.json */
void runSaveDockTest(const QMap<QString, QString> &)
{
	obs_frontend_save();
}

} // namespace

/* Request params:
 *   select        - optional: a device id to select in the camera list,
 *                   or "none" to clear the selection
 *   add           - optional: after the above, press the Add button
 *                   ("button"), or choose "camera", "local" or "toggle_default" in its menu
 *   filename      - where to write the {"found", "rows" ([{"text", "id",
 *               "store", "tooltip"}]), "device_names", "selected",
 *               "add_menu", "add_stores"} JSON result
 */
void registerPresetViewTest(PTZUITestHarness *harness)
{
	harness->registerTest(QStringLiteral("get_preset_view"), &runPresetViewTest);
	harness->registerTest(QStringLiteral("save_dock"), &runSaveDockTest);
}

/* save_dock request params: none. It has OBS save, so the dock writes its
 * config.json */
