/* PTZ UI test harness: reordering cameras
 *
 * Copyright 2026 Grant Likely <grant.likely@secretlab.ca>
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include "ui-test-harness.hpp"

#include <obs.hpp>
#include <obs-module.h>
#include <obs-frontend-api.h>
#include <QApplication>
#include <QComboBox>
#include <QWidget>

#include "circularlistview.hpp"
#include "ptz-list-model.hpp"
#include "ptz-controls.hpp"
#include "settings.hpp"

namespace {

/* The "deviceList" under a window: the camera list of the dock, in the main window, or of the
 * settings dialog, which is a window of its own */
CircularListView *cameraListIn(QWidget *window)
{
	return window ? window->findChild<CircularListView *>(QStringLiteral("deviceList")) : nullptr;
}

CircularListView *settingsCameraList()
{
	for (QWidget *w : QApplication::topLevelWidgets())
		if (auto dialog = qobject_cast<PTZSettings *>(w))
			return cameraListIn(dialog);
	return nullptr;
}

OBSDataArrayAutoRelease namesOf(CircularListView *view)
{
	OBSDataArrayAutoRelease array = obs_data_array_create();
	for (int row = 0; view && row < view->model()->rowCount(view->rootIndex()); row++) {
		OBSDataAutoRelease item = obs_data_create();
		auto index = view->model()->index(row, 0, view->rootIndex());
		obs_data_set_string(item, "text", qUtf8Printable(index.data(Qt::DisplayRole).toString()));
		obs_data_array_push_back(array, item);
	}
	return array;
}

void runReorderCamerasTest(const QMap<QString, QString> &params)
{
	QString filename = params.value(QStringLiteral("filename"));
	if (filename.isEmpty()) {
		blog(LOG_INFO, "[ptz-ui-test] reorder_cameras: missing filename");
		return;
	}

	auto *mainWindow = static_cast<QWidget *>(obs_frontend_get_main_window());
	auto *dockList = cameraListIn(mainWindow);
	auto *settingsList = settingsCameraList();
	bool inSettings = params.value(QStringLiteral("in")) == QStringLiteral("settings");
	auto *view = inSettings ? settingsList : dockList;

	OBSDataAutoRelease result = obs_data_create();
	obs_data_set_bool(result, "found", dockList && view);
	bool moved = false;

	if (dockList && view) {
		/* The cameras are given an order as a loaded one would be, by name */
		if (params.contains(QStringLiteral("set_order"))) {
			QStringList uuids;
			for (const auto &name : params.value(QStringLiteral("set_order")).split(QLatin1Char(','))) {
				auto index = ptzDeviceList->indexFromName(name);
				if (index.isValid())
					uuids << index.data(PTZListModel::DeviceUuidRole).toString();
			}
			ptzDeviceList->setDeviceOrder(uuids);
		}

		/* What loading a scene collection does with its saved order, with the saved file
		 * of one: as OBS hands the collection's "modules" to the plugin when it loads it */
		QString loadFrom = params.value(QStringLiteral("load_from"));
		if (!loadFrom.isEmpty()) {
			OBSDataAutoRelease collection = obs_data_create_from_json_file(qUtf8Printable(loadFrom));
			OBSDataAutoRelease modules = collection ? obs_data_get_obj(collection, "modules") : nullptr;
			OBSDataAutoRelease none = obs_data_create();
			PTZControls::loadCameraOrder(modules ? modules.Get() : none.Get());
		}

		if (params.contains(QStringLiteral("from")) && params.contains(QStringLiteral("to"))) {
			int from = params.value(QStringLiteral("from")).toInt();
			int to = params.value(QStringLiteral("to")).toInt();
			auto root = view->rootIndex();
			if (params.value(QStringLiteral("via")) == QStringLiteral("drop")) {
				/* `to` is a row to drop onto, in its lower half, or past the last
				 * row for one off the end of the list */
				view->setCurrentIndex(ptzDeviceList->index(from, 0, root));
				int rows = ptzDeviceList->rowCount(root);
				auto r = view->visualRect(ptzDeviceList->index(qMin(to, rows - 1), 0, root));
				QPoint pos(r.center().x(), r.bottom() - 1);
				if (to >= rows && r.bottom() + 2 < view->viewport()->height())
					pos = QPoint(r.center().x(), r.bottom() + 2);
				moved = view->dropCurrentRowAt(pos);
			} else {
				/* `to` is moveRow()'s own destination: the row to go before */
				moved = ptzDeviceList->moveRow(root, from, root, to);
			}
		}

		/* What a restart does to a scene collection's cameras: they are made again,
		 * from what was saved, by loading it again */
		if (params.value(QStringLiteral("reload")) == QStringLiteral("1")) {
			char *current = obs_frontend_get_current_scene_collection();
			QString name = QString::fromUtf8(current);
			bfree(current);
			obs_frontend_add_scene_collection("ptz-reorder-cameras-other");
			obs_frontend_set_current_scene_collection(qUtf8Printable(name));
		}

		obs_data_set_array(result, "devices", namesOf(dockList));
		obs_data_set_array(result, "settings_devices", namesOf(settingsList));
		OBSDataArrayAutoRelease order = obs_data_array_create();
		for (const QString &uuid : ptzDeviceList->deviceOrder()) {
			OBSDataAutoRelease item = obs_data_create();
			obs_data_set_string(item, "text", qUtf8Printable(uuid));
			obs_data_array_push_back(order, item);
		}
		obs_data_set_array(result, "uuids", order);
		obs_data_set_int(result, "selected_row", view->currentIndex().row());
		obs_data_set_bool(result, "dock_drag", dockList->dragEnabled());
		obs_data_set_bool(result, "settings_drag", settingsList && settingsList->dragEnabled());
	}
	obs_data_set_bool(result, "moved", moved);

	if (!obs_data_save_json_safe(result, qUtf8Printable(filename), "tmp", "bak"))
		blog(LOG_INFO, "[ptz-ui-test] reorder_cameras: failed to write %s", qUtf8Printable(filename));
}

} // namespace

/* Request params:
 *   in        - "dock" (the default) or "settings": which list a move is done in. The
 *               settings dialog has to be open, see open_settings_dialog
 *   set_order - optional: comma-separated camera names to hand to
 *               setDeviceOrder(), as a loaded scene collection does
 *   from, to  - optional: move the camera in row `from` to `to`
 *   via       - "model" (the default) moves it with the model's moveRow(), with `to` the
 *               row to go before. "drop" does what dropping it on row `to` does, in its
 *               lower half, or past the last row if `to` is beyond them
 *   load_from - optional: the path of a saved scene collection, whose saved order of the
 *               cameras is applied as loading it does
 *   reload    - "1": switch to another scene collection and back, which makes the cameras
 *               again from what was saved, as a restart does
 *   filename  - where to write the {"found", "moved", "devices", "settings_devices",
 *               "uuids", "selected_row", "dock_drag", "settings_drag"} JSON result. The
 *               lists of cameras are {"text"} rows, as the dock and the dialog show them */
void registerReorderCamerasTest(PTZUITestHarness *harness)
{
	harness->registerTest(QStringLiteral("reorder_cameras"), &runReorderCamerasTest);
}
