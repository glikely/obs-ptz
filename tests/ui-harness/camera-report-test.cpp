/* PTZ UI test harness: the camera report dialog
 *
 * Copyright 2026 Grant Likely <grant.likely@secretlab.ca>
 *
 * SPDX-License-Identifier: GPLv2
 */
#include "ui-test-harness.hpp"

#include <obs.hpp>
#include <obs-module.h>
#include <QApplication>
#include <QClipboard>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLabel>
#include <QPlainTextEdit>
#include <QProgressBar>
#include <QPushButton>
#include <QSaveFile>

#include "ptz-camera-report.hpp"
#include "ptz-list-model.hpp"

namespace {

PTZCameraReportDialog *findDialog()
{
	for (QWidget *w : QApplication::topLevelWidgets())
		if (auto dialog = qobject_cast<PTZCameraReportDialog *>(w))
			if (dialog->isVisible())
				return dialog;
	return nullptr;
}

/* Opens the dialog for a device, which starts its report; reads what it
 * shows; clicks one of its buttons ("copy" or "save" -- not "openIssue",
 * which would open a browser); or closes it */
void runCameraReportDialogTest(const QMap<QString, QString> &params)
{
	const QString action = params.value(QStringLiteral("action"));
	if (action == QStringLiteral("open")) {
		QModelIndex index = ptzUITestDeviceIndex(params.value(QStringLiteral("device")));
		if (!index.isValid()) {
			blog(LOG_INFO, "[ptz-ui-test] camera_report_dialog: missing/invalid device");
			return;
		}
		(new PTZCameraReportDialog(index.data(PTZListModel::DeviceUuidRole).toString()))->show();
		return;
	}

	PTZCameraReportDialog *dialog = findDialog();
	if (action == QStringLiteral("close")) {
		if (dialog)
			dialog->close();
		return;
	}
	if (action == QStringLiteral("click")) {
		auto button = dialog ? dialog->findChild<QPushButton *>(params.value(QStringLiteral("button")))
				     : nullptr;
		if (button)
			button->click();
		return;
	}

	QString filename = params.value(QStringLiteral("filename"));
	if (filename.isEmpty()) {
		blog(LOG_INFO, "[ptz-ui-test] camera_report_dialog: missing filename");
		return;
	}
	QJsonObject result;
	result["open"] = dialog != nullptr;
	if (dialog) {
		result["status"] = dialog->findChild<QLabel *>("status")->text();
		result["report"] = dialog->findChild<QPlainTextEdit *>("report")->toPlainText();
		auto progress = dialog->findChild<QProgressBar *>("progress");
		result["progress"] = progress->value();
		result["progress_max"] = progress->maximum();
		QJsonObject enabled;
		for (const char *name : {"save", "copy", "openIssue"})
			enabled[name] = dialog->findChild<QPushButton *>(name)->isEnabled();
		result["enabled"] = enabled;
		result["issue_url"] = dialog->issueUrl().toString(QUrl::FullyEncoded);
	}
	result["clipboard"] = QApplication::clipboard()->text();
	QSaveFile file(filename);
	if (!file.open(QIODevice::WriteOnly) || file.write(QJsonDocument(result).toJson()) < 0 || !file.commit())
		blog(LOG_INFO, "[ptz-ui-test] camera_report_dialog: failed to write %s", qUtf8Printable(filename));
}

} // namespace

/* camera_report_dialog request params:
 *   action    - "open", "read", "click" or "close"
 *   device - the device, by the UUID of its filter or the name of the source it is on
 *   button    - for "click", the button's name: "copy" or "save"
 *   filename  - for "read", where to write what the dialog shows, as JSON:
 *               {"open", "status", "report", "progress", "progress_max",
 *               "enabled": {"save", "copy", "openIssue"}, "issue_url",
 *               "clipboard"}
 */
void registerCameraReportTest(PTZUITestHarness *harness)
{
	harness->registerTest(QStringLiteral("camera_report_dialog"), &runCameraReportDialogTest);
}
