/* PTZ UI test harness: local preset tests
 *
 * Copyright 2026 Grant Likely <grant.likely@secretlab.ca>
 *
 * SPDX-License-Identifier: GPLv2
 */
#include "ui-test-harness.hpp"

#include <obs.hpp>
#include <obs-module.h>
#include <obs-frontend-api.h>

#include <QAbstractButton>
#include <QAction>
#include <QCheckBox>
#include <QComboBox>
#include <QDialogButtonBox>
#include <QListView>
#include <QPushButton>
#include <QWidget>

#include "ptz-list-model.hpp"
#include "ptz-preset-dialog.hpp"

namespace {

QModelIndex presetIndex(uint32_t deviceId, int presetId)
{
	QModelIndex parent = ptzDeviceList->indexFromDeviceId(deviceId);
	for (int row = 0; row < ptzDeviceList->rowCount(parent); row++) {
		QModelIndex index = ptzDeviceList->index(row, 0, parent);
		if (index.data(Qt::UserRole).toInt() == presetId)
			return index;
	}
	return QModelIndex();
}

/* Writes a preset's description, as the "ptz_preset_get" proc handler gives
 * it, under "preset", and every preset's {"id", "local"} under "presets" */
void writePresets(uint32_t deviceId, int presetId, const QString &filename)
{
	OBSDataAutoRelease result = obs_data_create();
	QModelIndex parent = ptzDeviceList->indexFromDeviceId(deviceId);

	OBSDataAutoRelease preset = obs_data_create();
	calldata cd = {};
	calldata_set_int(&cd, "id", presetId);
	calldata_set_ptr(&cd, "preset", preset.Get());
	ptzDeviceList->callDevice(parent, "ptz_preset_get", &cd);
	calldata_free(&cd);
	obs_data_set_obj(result, "preset", preset);

	OBSDataArrayAutoRelease presets = obs_data_array_create();
	for (int row = 0; row < ptzDeviceList->rowCount(parent); row++) {
		QModelIndex index = ptzDeviceList->index(row, 0, parent);
		OBSDataAutoRelease item = obs_data_create();
		obs_data_set_int(item, "id", index.data(Qt::UserRole).toInt());
		obs_data_set_bool(item, "local", index.data(PTZListModel::PresetLocalRole).toBool());
		obs_data_array_push_back(presets, item);
	}
	obs_data_set_array(result, "presets", presets);

	if (!obs_data_save_json_safe(result, qUtf8Printable(filename), "tmp", "bak"))
		blog(LOG_INFO, "[ptz-ui-test] local presets: failed to write %s", qUtf8Printable(filename));
}

bool deviceParams(const QMap<QString, QString> &params, const char *test, uint32_t &deviceId, QString &filename)
{
	bool ok = false;
	deviceId = params.value(QStringLiteral("device_id")).toUInt(&ok);
	filename = params.value(QStringLiteral("filename"));
	if (!ok || filename.isEmpty() || !ptzDeviceList->indexFromDeviceId(deviceId).isValid()) {
		blog(LOG_INFO, "[ptz-ui-test] %s: missing/invalid device_id or filename", test);
		return false;
	}
	return true;
}

/* Reports a preset, see writePresets() */
void runGetPresetTest(const QMap<QString, QString> &params)
{
	uint32_t deviceId;
	QString filename;
	if (!deviceParams(params, "get_preset", deviceId, filename))
		return;
	writePresets(deviceId, params.value(QStringLiteral("preset_id")).toInt(), filename);
}

/* Selects the device in the dock's camera list and triggers the dock's own
 * "Add Local Preset" or "Add Camera Preset" action, then reports the new
 * preset (the last row) */
void runAddPresetTest(const QMap<QString, QString> &params)
{
	uint32_t deviceId;
	QString filename;
	if (!deviceParams(params, "add_preset", deviceId, filename))
		return;
	auto *mainWindow = static_cast<QWidget *>(obs_frontend_get_main_window());
	auto *deviceList = mainWindow ? mainWindow->findChild<QListView *>(QStringLiteral("deviceList")) : nullptr;
	bool local = params.value(QStringLiteral("local")) == QStringLiteral("1");
	auto *action = mainWindow ? mainWindow->findChild<QAction *>(local ? QStringLiteral("actionPresetAddLocal")
									   : QStringLiteral("actionPresetAddCamera"))
				  : nullptr;
	if (!deviceList || !action) {
		blog(LOG_INFO, "[ptz-ui-test] add_preset: dock not found");
		return;
	}
	QModelIndex parent = ptzDeviceList->indexFromDeviceId(deviceId);
	deviceList->setCurrentIndex(parent);
	action->trigger();
	int rows = ptzDeviceList->rowCount(parent);
	int presetId = rows > 0 ? ptzDeviceList->index(rows - 1, 0, parent).data(Qt::UserRole).toInt() : -1;
	writePresets(deviceId, presetId, filename);
}

/* Opens the Edit Preset dialog on a preset, sets the storage ("local" or
 * "camera"), unchecks the recall of each key in "uncheck" (comma separated),
 * then presses OK, and reports the preset as it is afterwards */
void runEditPresetDialogTest(const QMap<QString, QString> &params)
{
	uint32_t deviceId;
	QString filename;
	if (!deviceParams(params, "edit_preset_dialog", deviceId, filename))
		return;
	int presetId = params.value(QStringLiteral("preset_id")).toInt();
	QModelIndex index = presetIndex(deviceId, presetId);
	if (!index.isValid()) {
		blog(LOG_INFO, "[ptz-ui-test] edit_preset_dialog: preset %d not found", presetId);
		return;
	}

	auto *dialog = new PTZPresetDialog(index);
	QString storage = params.value(QStringLiteral("storage"));
	if (!storage.isEmpty()) {
		auto *combo = dialog->findChild<QComboBox *>(QStringLiteral("presetStorage"));
		if (combo)
			combo->setCurrentIndex(storage == QStringLiteral("local") ? 1 : 0);
	}
	for (const QString &key : params.value(QStringLiteral("uncheck")).split(',', Qt::SkipEmptyParts)) {
		auto *box = dialog->findChild<QCheckBox *>(QStringLiteral("recall_") + key);
		if (box)
			box->setChecked(false);
	}
	auto *buttons = dialog->findChild<QDialogButtonBox *>();
	if (buttons)
		buttons->button(QDialogButtonBox::Ok)->click();
	else
		dialog->close();

	writePresets(deviceId, presetId, filename);
}

} // namespace

/* Request params:
 *   get_preset         - device_id, preset_id, filename
 *   add_preset         - device_id, local ("1" or "0"), filename
 *   edit_preset_dialog - device_id, preset_id, storage (optional),
 *                        uncheck (optional), filename
 * Each writes {"preset": <ptz_preset_get>, "presets": [{"id", "local"}]}
 */
void registerPresetLocalTest(PTZUITestHarness *harness)
{
	harness->registerTest(QStringLiteral("get_preset"), &runGetPresetTest);
	harness->registerTest(QStringLiteral("add_preset"), &runAddPresetTest);
	harness->registerTest(QStringLiteral("edit_preset_dialog"), &runEditPresetDialogTest);
}
