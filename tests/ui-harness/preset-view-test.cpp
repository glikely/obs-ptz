/* PTZ UI test harness: preset list view test
 *
 * Copyright 2026 Grant Likely <grant.likely@secretlab.ca>
 *
 * SPDX-License-Identifier: GPLv2
 */
#include "ui-test-harness.hpp"

#include <obs.hpp>
#include <obs-module.h>
#include <obs-frontend-api.h>
#include <QList>
#include <QListView>
#include <QWidget>

#include "ptz-list-model.hpp"
#include "ptz.h"

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
 *   selected       - whether a device is selected in the camera list */
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
		/* Adding or removing a device resets the model */
		QString addDevice = params.value(QStringLiteral("add_device"));
		if (!addDevice.isEmpty()) {
			/* A device is a filter on a source: make the source, and
			 * hold it, since nothing else does */
			static QList<OBSSource> held;
			OBSDataAutoRelease config = obs_data_create();
			obs_data_set_string(config, "type", "visca-over-ip");
			obs_data_set_string(config, "host", "127.0.0.1");
			OBSSourceAutoRelease source =
				obs_source_create("color_source_v3", qUtf8Printable(addDevice), nullptr, nullptr);
			if (source) {
				held.append(OBSSource(source.Get()));
				OBSSourceAutoRelease filter = ptz_device_create_filter(source, config);
			}
		}
		QString removeDevice = params.value(QStringLiteral("remove_device"));
		if (!removeDevice.isEmpty())
			ptzDeviceList->removeDevice(ptzDeviceList->indexFromName(removeDevice));

		QString select = params.value(QStringLiteral("select"));
		if (select == QStringLiteral("none")) {
			deviceList->selectionModel()->setCurrentIndex(QModelIndex(), QItemSelectionModel::Clear);
		} else if (!select.isEmpty()) {
			deviceList->setCurrentIndex(ptzDeviceList->indexFromName(select));
		}

		OBSDataArrayAutoRelease rows = obs_data_array_create();
		auto *model = presetList->model();
		auto root = presetList->rootIndex();
		for (int row = 0; model && row < model->rowCount(root); row++) {
			OBSDataAutoRelease item = obs_data_create();
			obs_data_set_string(
				item, "text",
				qUtf8Printable(model->index(row, 0, root).data(Qt::DisplayRole).toString()));
			obs_data_array_push_back(rows, item);
		}
		obs_data_set_array(result, "rows", rows);

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

/* Adds a preset to a device, as a user does from the preset list: a change the
 * device makes itself, which its settings did not give it */
void runAddPresetTest(const QMap<QString, QString> &params)
{
	QModelIndex index = ptzUITestDeviceIndex(params.value(QStringLiteral("device")));
	if (!index.isValid()) {
		blog(LOG_INFO, "[ptz-ui-test] add_preset: device not found");
		return;
	}
	ptzDeviceList->insertRows(ptzDeviceList->rowCount(index), 1, index);
	blog(LOG_INFO, "[ptz-ui-test] add_preset: done");
}

} // namespace

/* Request params:
 *   add_device    - optional: add a device with this name first. It has no
 *                   source and no camera
 *   remove_device - optional: remove the device with this name first
 *   select        - optional: a device id to select in the camera list
 *                   (after the above), or "none" to clear the selection
 *   filename      - where to write the {"found", "rows", "device_names",
 *               "selected"} JSON result
 */
void registerPresetViewTest(PTZUITestHarness *harness)
{
	harness->registerTest(QStringLiteral("get_preset_view"), &runPresetViewTest);
	harness->registerTest(QStringLiteral("add_preset"), &runAddPresetTest);
}

/* add_preset request params:
 *   device - the device, by the UUID of its filter or the name of the source it is on
 */
