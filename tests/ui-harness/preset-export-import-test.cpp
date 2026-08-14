/* PTZ UI test harness: preset export/import test
 *
 * Copyright 2026 Grant Likely <grant.likely@secretlab.ca>
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
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
	QString deviceName = params.value(QStringLiteral("device"));
	bool deviceOk = !deviceName.isEmpty();
	QString filename = params.value(QStringLiteral("filename"));
	if (!deviceOk || filename.isEmpty()) {
		blog(LOG_INFO, "[ptz-ui-test] %s: missing/invalid device or filename", methodName);
		return;
	}

	auto *mainWindow = static_cast<QWidget *>(obs_frontend_get_main_window());
	auto *deviceList = mainWindow ? mainWindow->findChild<QListView *>(QStringLiteral("deviceList")) : nullptr;
	auto *ptzctrls = mainWindow ? mainWindow->findChild<QFrame *>(QString::fromLatin1("PTZControls")) : nullptr;
	if (!deviceList || !ptzctrls) {
		blog(LOG_INFO, "[ptz-ui-test] %s: PTZControls object", methodName);
		return;
	}

	QModelIndex index = ptzUITestDeviceIndex(deviceName);
	if (!index.isValid()) {
		blog(LOG_INFO, "[ptz-ui-test] %s: device %s not found", methodName, qUtf8Printable(deviceName));
		return;
	}
	deviceList->setCurrentIndex(index);

	QMetaObject::invokeMethod(ptzctrls, methodName, Q_ARG(QString, filename));

	blog(LOG_INFO, "[ptz-ui-test] %s triggered device=%s filename=%s", methodName, qUtf8Printable(deviceName),
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
 *   device - the device, by the UUID of its filter or the name of the source it is on
 *   filename  - the path actionPresetExport/actionPresetImport should
 *               export to/import from, bypassing the real QFileDialog
 *               (see runPresetIOTest()'s own comment above)
 */
void registerPresetExportImportTest(PTZUITestHarness *harness)
{
	harness->registerTest(QStringLiteral("export_presets"), &runPresetExportTest);
	harness->registerTest(QStringLiteral("import_presets"), &runPresetImportTest);
}
