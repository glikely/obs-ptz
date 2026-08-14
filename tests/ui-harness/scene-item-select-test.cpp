/* PTZ UI test harness: selecting a source in the scene selects its camera
 *
 * Copyright 2026 Grant Likely <grant.likely@secretlab.ca>
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "ui-test-harness.hpp"

#include <obs.hpp>
#include <obs-module.h>
#include <obs-frontend-api.h>
#include <QListView>
#include <QWidget>

#include "ptz-controls.hpp"
#include "ptz-list-model.hpp"

namespace {

/* Selects a source's item in a scene, as a click in the Sources dock does,
 * after setting whether the dock follows the selection (autoselect). */
void runSelectSceneItem(const QMap<QString, QString> &params)
{
	auto *controls = PTZControls::getInstance();
	if (params.contains(QStringLiteral("autoselect")))
		controls->setAutoselectEnabled(params.value(QStringLiteral("autoselect")) == QStringLiteral("true"));

	if (!params.contains(QStringLiteral("scene")))
		return;

	OBSSourceAutoRelease sceneSource =
		obs_get_source_by_name(qUtf8Printable(params.value(QStringLiteral("scene"))));
	obs_scene_t *scene = obs_scene_from_source(sceneSource);
	obs_sceneitem_t *item =
		scene ? obs_scene_find_source(scene, qUtf8Printable(params.value(QStringLiteral("source")))) : nullptr;
	if (!item) {
		blog(LOG_INFO, "[ptz-ui-test] select_scene_item: no such item");
		return;
	}
	/* New items start out selected, and selecting a selected item is no event */
	obs_sceneitem_select(item, false);
	obs_sceneitem_select(item, true);
}

/* Reports, as JSON, {"found": whether the dock's camera list was found,
 * "selected": whether the device is the one selected in it} */
void runGetDockSelection(const QMap<QString, QString> &params)
{
	QString filename = params.value(QStringLiteral("filename"));
	if (filename.isEmpty()) {
		blog(LOG_INFO, "[ptz-ui-test] get_dock_selection: missing filename");
		return;
	}
	auto *mainWindow = static_cast<QWidget *>(obs_frontend_get_main_window());
	auto *deviceList = mainWindow ? mainWindow->findChild<QListView *>(QStringLiteral("deviceList")) : nullptr;
	OBSDataAutoRelease result = obs_data_create();
	obs_data_set_bool(result, "found", deviceList);
	if (deviceList) {
		auto index = ptzUITestDeviceIndex(params.value(QStringLiteral("device")));
		obs_data_set_bool(result, "selected", index.isValid() && deviceList->currentIndex() == index);
	}
	if (!obs_data_save_json_safe(result, qUtf8Printable(filename), "tmp", "bak"))
		blog(LOG_INFO, "[ptz-ui-test] get_dock_selection: failed to write %s", qUtf8Printable(filename));
}

} // namespace

/* Request params:
 *   select_scene_item:
 *     scene, source - the item to select; none, if only to set autoselect
 *     autoselect    - "true" or "false", to set the dock's autoselect first
 *   get_dock_selection:
 *     device   - the device, by the UUID of its filter or the name of its source
 *     filename - where to write the {"found", "selected"} JSON result
 */
void registerSceneItemSelectTest(PTZUITestHarness *harness)
{
	harness->registerTest(QStringLiteral("select_scene_item"), &runSelectSceneItem);
	harness->registerTest(QStringLiteral("get_dock_selection"), &runGetDockSelection);
}
