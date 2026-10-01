/* Camera report dialog
 *
 * Copyright 2026 Grant Likely <grant.likely@secretlab.ca>
 *
 * SPDX-License-Identifier: GPLv2
 */
#include "ptz-camera-report.hpp"

#include <obs-module.h>
#include <obs-frontend-api.h>
#include <QClipboard>
#include <QDesktopServices>
#include <QDir>
#include <QFileDialog>
#include <QFontDatabase>
#include <QGuiApplication>
#include <QJsonDocument>
#include <QJsonObject>
#include <QMessageBox>
#include <QRegularExpression>
#include <QSaveFile>
#include <QUrlQuery>

#include "ptz.h"
#include "ptz-list-model.hpp"
#include "ui_ptz-camera-report.h"

/* Where reports are sent in: the plugin's issue form for them, see
 * .github/ISSUE_TEMPLATE/camera-report.yml */
static const char *issue_form = "https://github.com/glikely/obs-ptz/issues/new";

PTZCameraReportDialog::PTZCameraReportDialog(uint32_t device_id, QWidget *parent)
	: QDialog(parent),
	  ui(new Ui_PTZCameraReport),
	  deviceId(device_id)
{
	/* So the text in the .ui file, which is the locale's keys, is translated */
	obs_frontend_push_ui_translation(obs_module_get_string);
	ui->setupUi(this);
	obs_frontend_pop_ui_translation();
	setAttribute(Qt::WA_DeleteOnClose);
	ui->report->setFont(QFontDatabase::systemFont(QFontDatabase::FixedFont));
	connect(ui->save, &QPushButton::clicked, this, &PTZCameraReportDialog::save);
	connect(ui->copy, &QPushButton::clicked, this, &PTZCameraReportDialog::copy);
	connect(ui->openIssue, &QPushButton::clicked, this, &PTZCameraReportDialog::openIssue);

	connect(ptzDeviceList, &PTZListModel::deviceStateUpdated, this, &PTZCameraReportDialog::stateUpdated);

	/* A camera that isn't connected can't be asked anything */
	QModelIndex index = ptzDeviceList->indexFromDeviceId(deviceId);
	OBSDataAutoRelease state = obs_data_create();
	if (index.isValid())
		ptzDeviceList->saveState(index, state.Get());
	if (!obs_data_get_bool(state, "connected")) {
		ui->progress->setRange(0, 1);
		ui->status->setText(obs_module_text("PTZ.CameraReport.NotConnected"));
		return;
	}
	calldata_t cd = {};
	calldata_set_string(&cd, "name", "camera_report");
	ptzDeviceList->callDevice(index, "ptz_trigger", &cd);
	calldata_free(&cd);
}

PTZCameraReportDialog::~PTZCameraReportDialog()
{
	delete ui;
}

/* How far the device has got with the report, and the report once it has
 * made it */
void PTZCameraReportDialog::stateUpdated(uint32_t device_id, OBSData changed)
{
	if (device_id != deviceId || !obs_data_has_user_value(changed, "camera_report"))
		return;
	OBSDataAutoRelease progressed = obs_data_get_obj(changed, "camera_report");
	if (obs_data_get_bool(progressed, "running")) {
		int done = (int)obs_data_get_int(progressed, "done");
		int total = (int)obs_data_get_int(progressed, "total");
		ui->progress->setRange(0, total);
		ui->progress->setValue(done);
		ui->status->setText(QString(obs_module_text("PTZ.CameraReport.Progress")).arg(done).arg(total));
		return;
	}
	ui->progress->setRange(0, 1);
	ui->progress->setValue(1);
	if (strcmp(obs_data_get_string(progressed, "error"), "standby") == 0) {
		ui->status->setText(obs_module_text("PTZ.CameraReport.Standby"));
		return;
	}
	showReport();
}

void PTZCameraReportDialog::showReport()
{
	calldata_t cd = {};
	ptzDeviceList->callDevice(ptzDeviceList->indexFromDeviceId(deviceId), "ptz_get_camera_report", &cd);
	const char *text = calldata_string(&cd, "report");
	report = QString::fromUtf8(text ? text : "");
	calldata_free(&cd);
	if (report.isEmpty()) {
		ui->status->setText(obs_module_text("PTZ.CameraReport.Failed"));
		return;
	}

	QJsonObject camera = QJsonDocument::fromJson(report.toUtf8()).object()["camera"].toObject();
	QStringList name;
	for (const char *key : {"vendor_name", "model_name"}) {
		if (camera.contains(key))
			name += camera[key].toString();
	}
	if (name.isEmpty() && camera.contains("vendor_id"))
		name += camera["vendor_id"].toString() + ":" + camera["model_id"].toString();
	cameraName = name.join(" ");

	ui->report->setPlainText(report);
	char *profiles = obs_module_config_path("visca-profiles");
	ui->status->setText(QString(obs_module_text("PTZ.CameraReport.Done")).arg(QDir::toNativeSeparators(profiles)));
	bfree(profiles);
	for (auto button : {ui->save, ui->copy, ui->openIssue})
		button->setEnabled(true);
}

void PTZCameraReportDialog::save()
{
	QString suggested = "camera-report";
	if (!cameraName.isEmpty())
		suggested += "-" + cameraName.toLower().replace(QRegularExpression("[^a-z0-9]+"), "-");
	QString filename = QFileDialog::getSaveFileName(this, obs_module_text("PTZ.CameraReport.Save"),
							QDir::home().filePath(suggested + ".json"), "JSON (*.json)");
	if (filename.isEmpty())
		return;
	QSaveFile file(filename);
	if (!file.open(QIODevice::WriteOnly) || file.write(report.toUtf8()) < 0 || !file.commit())
		QMessageBox::warning(this, obs_module_text("PTZ.CameraReport.Title"),
				     QString(obs_module_text("PTZ.CameraReport.SaveFailed")).arg(filename));
}

void PTZCameraReportDialog::copy()
{
	QGuiApplication::clipboard()->setText(report);
	ui->status->setText(obs_module_text("PTZ.CameraReport.Copied"));
}

QUrl PTZCameraReportDialog::issueUrl() const
{
	QUrlQuery query;
	query.addQueryItem("template", "camera-report.yml");
	if (!cameraName.isEmpty()) {
		query.addQueryItem("title", "Camera report: " + cameraName);
		query.addQueryItem("camera", cameraName);
	}
	query.addQueryItem("plugin-version", ptz_plugin_version);
	QUrl url(issue_form);
	url.setQuery(query);
	return url;
}

/* The report is too long to go in the link, so it is copied, for its user
 * to paste in */
void PTZCameraReportDialog::openIssue()
{
	QGuiApplication::clipboard()->setText(report);
	ui->status->setText(obs_module_text("PTZ.CameraReport.CopiedForIssue"));
	QDesktopServices::openUrl(issueUrl());
}
