/* PTZ UI test harness: backups of removed devices, and adding and removing
 * devices from the settings dialog
 *
 * Copyright 2026 Grant Likely <grant.likely@secretlab.ca>
 *
 * SPDX-License-Identifier: GPLv2
 */
#include "ui-test-harness.hpp"

#include <obs.hpp>
#include <obs-module.h>
#include <QApplication>
#include <QComboBox>
#include <QDialog>
#include <QMessageBox>
#include <QPushButton>
#include <QStandardItemModel>
#include <QTimer>

#include "ptz-list-model.hpp"
#include "ptz-controls.hpp"
#include "settings.hpp"
#include "ptz.h"

namespace {

PTZSettings *findDialog()
{
	for (QWidget *w : QApplication::topLevelWidgets())
		if (auto dialog = qobject_cast<PTZSettings *>(w))
			return dialog;
	return nullptr;
}

/* Writes ptz_device_backups_get() as {"devices": [...]} */
void runGetDeviceBackupsTest(const QMap<QString, QString> &params)
{
	QString filename = params.value(QStringLiteral("filename"));
	if (filename.isEmpty())
		return;
	OBSDataArrayAutoRelease backups = ptz_device_backups_get();
	OBSDataAutoRelease result = obs_data_create();
	obs_data_set_array(result, "devices", backups);
	obs_data_save_json_safe(result, qUtf8Printable(filename), "tmp", "bak");
}

/* Adds a device as a user would, through the settings dialog's Add: picks
 * `source` in the add dialog, then the protocol that makes `type` or the
 * backup of the device that was on the source called `restore`, or, with
 * neither, whatever the dialog picked for the source, and presses OK. Writes
 * what the dialog offered: {"sources": [...], "choices": [...], "chosen"} */
void runAddDeviceTest(const QMap<QString, QString> &params)
{
	QString filename = params.value(QStringLiteral("filename"));
	QString source = params.value(QStringLiteral("source"));
	QString type = params.value(QStringLiteral("type"));
	QString restore = params.value(QStringLiteral("restore"));
	ptz_settings_show();
	PTZSettings *dialog = findDialog();
	if (!dialog || filename.isEmpty())
		return;

	/* The add dialog is modal: answer it from inside its own event loop */
	QTimer::singleShot(0, [=]() {
		auto add = qobject_cast<QDialog *>(QApplication::activeModalWidget());
		auto sources = add ? add->findChild<QComboBox *>(QStringLiteral("sourceList")) : nullptr;
		auto devices = add ? add->findChild<QComboBox *>(QStringLiteral("deviceTypeList")) : nullptr;
		OBSDataAutoRelease result = obs_data_create();
		if (!sources || !devices) {
			blog(LOG_INFO, "[ptz-ui-test] add_device: no add dialog");
			if (add)
				add->reject();
			obs_data_save_json_safe(result, qUtf8Printable(filename), "tmp", "bak");
			return;
		}
		OBSDataArrayAutoRelease sourceItems = obs_data_array_create();
		for (int i = 0; i < sources->count(); i++) {
			OBSDataAutoRelease item = obs_data_create();
			obs_data_set_string(item, "name", qUtf8Printable(sources->itemText(i)));
			obs_data_array_push_back(sourceItems, item);
		}
		sources->setCurrentIndex(sources->findText(source));
		OBSDataArrayAutoRelease choiceItems = obs_data_array_create();
		for (int i = 0; i < devices->count(); i++) {
			if (devices->itemText(i).isEmpty())
				continue; /* the separator */
			auto model = qobject_cast<QStandardItemModel *>(devices->model());
			bool heading = model && !(model->item(i)->flags() & Qt::ItemIsSelectable);
			OBSDataAutoRelease item = obs_data_create();
			obs_data_set_string(item, "name", qUtf8Printable(devices->itemText(i)));
			obs_data_set_bool(item, "heading", heading);
			obs_data_array_push_back(choiceItems, item);
		}
		int row = devices->currentIndex();
		if (!type.isEmpty())
			row = devices->findData(type, Qt::UserRole + 1);
		else if (!restore.isEmpty())
			row = devices->findData(restore, Qt::UserRole + 2);
		devices->setCurrentIndex(row);
		obs_data_set_array(result, "sources", sourceItems);
		obs_data_set_array(result, "choices", choiceItems);
		obs_data_set_string(result, "chosen", qUtf8Printable(devices->currentText()));
		if (sources->currentText() == source && row >= 0)
			add->accept();
		else
			add->reject();
		obs_data_save_json_safe(result, qUtf8Printable(filename), "tmp", "bak");
	});

	dialog->addDevice();
}

/* Removes the device called `name` as a user would, with the settings
 * dialog's Remove, answering Yes to its question */
void runRemoveDeviceTest(const QMap<QString, QString> &params)
{
	QModelIndex index = ptzDeviceList->indexFromName(params.value(QStringLiteral("name")));
	if (!index.isValid()) {
		blog(LOG_INFO, "[ptz-ui-test] remove_device: no such device");
		return;
	}
	ptz_settings_show(index);
	PTZSettings *dialog = findDialog();
	if (!dialog)
		return;
	QTimer::singleShot(0, []() {
		auto box = qobject_cast<QMessageBox *>(QApplication::activeModalWidget());
		if (box && box->button(QMessageBox::Yes))
			box->button(QMessageBox::Yes)->click();
	});
	dialog->on_removePTZ_clicked();
}

} // namespace

/* get_device_backups: filename - where to write {"devices": [...]}
 * add_device: filename, source, and either type - the protocol's new device
 *   type ("visca-over-ip", say), or restore - the source name of the backup
 *   to restore; with neither, what the dialog picks for the source
 * remove_device: name - the device to remove
 */
void registerDeviceBackupTest(PTZUITestHarness *harness)
{
	harness->registerTest(QStringLiteral("get_device_backups"), &runGetDeviceBackupsTest);
	harness->registerTest(QStringLiteral("add_device"), &runAddDeviceTest);
	harness->registerTest(QStringLiteral("remove_device"), &runRemoveDeviceTest);
}
