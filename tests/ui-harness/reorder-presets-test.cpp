/* PTZ UI test harness: reordering presets
 *
 * Copyright 2026 Grant Likely <grant.likely@secretlab.ca>
 *
 * SPDX-License-Identifier: GPLv2
 */
#include "ui-test-harness.hpp"

#include <obs.hpp>
#include <obs-module.h>
#include <obs-frontend-api.h>
#include <QWidget>
#include <QAction>

#include "circularlistview.hpp"
#include "ptz-list-model.hpp"

namespace {

void runReorderPresetsTest(const QMap<QString, QString> &params)
{
	QString filename = params.value(QStringLiteral("filename"));
	if (filename.isEmpty()) {
		blog(LOG_INFO, "[ptz-ui-test] reorder_presets: missing filename");
		return;
	}

	auto *mainWindow = static_cast<QWidget *>(obs_frontend_get_main_window());
	auto *deviceList = mainWindow ? mainWindow->findChild<CircularListView *>(QStringLiteral("deviceList"))
				      : nullptr;
	auto *presetList = mainWindow ? mainWindow->findChild<CircularListView *>(QStringLiteral("presetListView"))
				      : nullptr;

	OBSDataAutoRelease result = obs_data_create();
	obs_data_set_bool(result, "found", deviceList && presetList);
	bool moved = false;

	if (deviceList && presetList) {
		if (params.contains(QStringLiteral("grid")))
			if (auto *grid = mainWindow->findChild<QAction *>(QStringLiteral("actionPresetGridView")))
				grid->setChecked(params.value(QStringLiteral("grid")) == QStringLiteral("1"));
		QString select = params.value(QStringLiteral("select"));
		if (!select.isEmpty())
			deviceList->setCurrentIndex(ptzUITestDeviceIndex(select));

		auto root = presetList->rootIndex();
		if (params.contains(QStringLiteral("from")) && params.contains(QStringLiteral("to"))) {
			int from = params.value(QStringLiteral("from")).toInt();
			int to = params.value(QStringLiteral("to")).toInt();
			if (params.value(QStringLiteral("via")) == QStringLiteral("drop")) {
				/* `to` is a row to drop onto, in its lower half, or past
				 * the last row for one off the end of the list */
				presetList->setCurrentIndex(ptzDeviceList->index(from, 0, root));
				int rows = ptzDeviceList->rowCount(root);
				auto r = presetList->visualRect(ptzDeviceList->index(qMin(to, rows - 1), 0, root));
				QPoint pos(r.center().x(), r.bottom() - 1);
				/* Below the last row if there is room there */
				if (to >= rows && r.bottom() + 2 < presetList->viewport()->height())
					pos = QPoint(r.center().x(), r.bottom() + 2);
				moved = presetList->dropCurrentRowAt(pos);
			} else {
				/* `to` is moveRow()'s own destination: the row to go before */
				moved = ptzDeviceList->moveRow(root, from, root, to);
			}
		}

		OBSDataArrayAutoRelease presets = obs_data_array_create();
		for (int row = 0; row < ptzDeviceList->rowCount(root); row++) {
			OBSDataAutoRelease item = obs_data_create();
			obs_data_set_string(
				item, "text",
				qUtf8Printable(ptzDeviceList->index(row, 0, root).data(Qt::DisplayRole).toString()));
			obs_data_array_push_back(presets, item);
		}
		obs_data_set_array(result, "presets", presets);
		obs_data_set_int(result, "selected_row", presetList->currentIndex().row());
		/* What the lists let be dragged: only the presets */
		obs_data_set_bool(result, "presets_drag", presetList->dragEnabled());
		obs_data_set_bool(result, "devices_drag", deviceList->dragEnabled());
		obs_data_set_bool(result, "preset_flags_drag",
				  root.isValid() && ptzDeviceList->rowCount(root) &&
					  (ptzDeviceList->flags(ptzDeviceList->index(0, 0, root)) &
					   Qt::ItemIsDragEnabled));
	}
	obs_data_set_bool(result, "moved", moved);

	if (!obs_data_save_json_safe(result, qUtf8Printable(filename), "tmp", "bak"))
		blog(LOG_INFO, "[ptz-ui-test] reorder_presets: failed to write %s", qUtf8Printable(filename));
}

} // namespace

/* Request params:
 *   select   - optional: a device to select in the camera list, by name. The
 *              preset list shows its presets
 *   grid     - optional: "1" for the grid view of the presets, "0" for the list
 *   from, to - optional: move the preset in row `from` to `to`
 *   via      - "model" (the default) moves it with the model's moveRow(),
 *              with `to` the row to go before. "drop" does what dropping it on
 *              row `to` does, in its lower half, or past the last row if `to`
 *              is beyond them
 *   filename - where to write the {"found", "moved", "presets", "selected_row",
 *              "presets_drag", "devices_drag", "preset_flags_drag"} JSON result.
 *              "presets" is {"text"} rows */
void registerReorderPresetsTest(PTZUITestHarness *harness)
{
	harness->registerTest(QStringLiteral("reorder_presets"), &runReorderPresetsTest);
}
