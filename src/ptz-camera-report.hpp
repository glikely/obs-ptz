/* Camera report dialog
 *
 * Copyright 2026 Grant Likely <grant.likely@secretlab.ca>
 *
 * SPDX-License-Identifier: GPLv2
 */
#pragma once

#include <obs.hpp>
#include <QDialog>
#include <QUrl>

class Ui_PTZCameraReport;

/* Makes a device's camera report (its "camera_report" trigger), shows it,
 * and lets its user save it, copy it, or open the issue form to send it in
 * with. The plugin sends it nowhere itself. */
class PTZCameraReportDialog : public QDialog {
	Q_OBJECT

	Ui_PTZCameraReport *ui;
	QString deviceUuid;
	QString report;
	QString cameraName;

	void stateUpdated(const QString &uuid, OBSData changed);
	void showReport();
	void save();
	void copy();
	void openIssue();

public:
	PTZCameraReportDialog(const QString &uuid, QWidget *parent = nullptr);
	~PTZCameraReportDialog();
	/* The issue form, with what it can be filled in with but the report,
	 * which is too long to go in a link */
	QUrl issueUrl() const;
};
