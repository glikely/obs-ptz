/* PTZ UI test harness: preset export/import test
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
#include <QListView>
#include <QAction>

#include "ptz-list-model.hpp"

namespace {

/* Drives PTZControls's real "Export Presets..."/"Import Presets..."
 * context-menu actions (on_actionPresetExport_triggered()/
 * on_actionPresetImport_triggered() in src/ptz-controls.cpp) end to
 * end: selects the requested device in the real deviceList view, then
 * triggers the real actionPresetExport/actionPresetImport QAction --
 * this is the only way this feature is tested (see
 * tests/obs-integration/test_preset_import_export.py's own docstring
 * for why there's deliberately no lower-level bypass).
 *
 * The one thing this still can't drive is the real QFileDialog itself:
 * see on_actionPresetExport_triggered()'s own comment for why (and how
 * PTZ_UI_TEST_PRESET_EXPORT_FILE/PTZ_UI_TEST_PRESET_IMPORT_FILE let it
 * skip that dialog here). */
void runPresetIOTest(const QMap<QString, QString> &params, const char *methodName)
{
	bool deviceIdOk = false;
	uint32_t deviceId = params.value(QStringLiteral("device_id")).toUInt(&deviceIdOk);
	QString filename = params.value(QStringLiteral("filename"));
	if (!deviceIdOk || filename.isEmpty()) {
		blog(LOG_INFO, "[ptz-ui-test] %s: missing/invalid device_id or filename", methodName);
		return;
	}

	auto *mainWindow = static_cast<QWidget *>(obs_frontend_get_main_window());
	auto *deviceList = mainWindow ? mainWindow->findChild<QListView *>(QStringLiteral("deviceList")) : nullptr;
	auto *ptzctrls = mainWindow ? mainWindow->findChild<QFrame *>(QString::fromLatin1("PTZControls")) : nullptr;
	if (!deviceList || !ptzctrls) {
		blog(LOG_INFO, "[ptz-ui-test] %s: PTZControls object", methodName);
		return;
	}

	QModelIndex index = ptzDeviceList->indexFromDeviceId(deviceId);
	if (!index.isValid()) {
		blog(LOG_INFO, "[ptz-ui-test] %s: device_id %u not found", methodName, deviceId);
		return;
	}
	deviceList->setCurrentIndex(index);

	QMetaObject::invokeMethod(ptzctrls, methodName, Q_ARG(QString, filename));

	blog(LOG_INFO, "[ptz-ui-test] %s triggered device_id=%u filename=%s", methodName, deviceId,
	     qUtf8Printable(filename));
}

void runPresetExportTest(const QMap<QString, QString> &params)
{
	runPresetIOTest(params, "on_actionPresetExport_triggered");
}

void runPresetImportTest(const QMap<QString, QString> &params)
{
	runPresetIOTest(params, "on_actionPresetImport_triggered");
}

} // namespace

/* Request params:
 *   device_id - the target device's numeric id (PTZDevice::id, same
 *               space tests/obs-integration/conftest.py's device_ids
 *               uses)
 *   filename  - the path actionPresetExport/actionPresetImport should
 *               export to/import from, bypassing the real QFileDialog
 *               (see runPresetIOTest()'s own comment above)
 */
void registerPresetExportImportTest(PTZUITestHarness *harness)
{
	harness->registerTest(QStringLiteral("export_presets"), &runPresetExportTest);
	harness->registerTest(QStringLiteral("import_presets"), &runPresetImportTest);
}
