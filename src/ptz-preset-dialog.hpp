/* Dialog for editing one PTZ preset
 *
 * Copyright 2026 Grant Likely <grant.likely@secretlab.ca>
 *
 * SPDX-License-Identifier: GPLv2
 */
#pragma once

#include <QDialog>
#include <QMap>
#include <QPersistentModelIndex>
#include <QVariantMap>
#include <obs.hpp>

class QCheckBox;
class QComboBox;
class QGridLayout;
class QGroupBox;
class QLabel;
class QLineEdit;
class QPushButton;

/* Edits a preset of the PTZListModel: its name, whether it is stored on the
 * camera or locally, and for a local one, which of the values it captured
 * it recalls, and what they are. Nothing is changed until it is accepted,
 * apart from "Save Current State", which captures the camera's state into
 * the preset there and then, as the dock's Save does. */
class PTZPresetDialog : public QDialog {
	Q_OBJECT

public:
	PTZPresetDialog(const QModelIndex &presetIndex, QWidget *parent = nullptr);

	/* The label a state key is shown with */
	static QString keyLabel(const QString &key);

private:
	void load();
	void showValues();
	void updateStorage();
	void saveCurrentState();
	void apply();

	QPersistentModelIndex m_index;
	QLineEdit *m_name;
	QComboBox *m_storage;
	QGroupBox *m_valuesGroup;
	QGridLayout *m_valuesGrid;
	QLabel *m_empty;
	QPushButton *m_capture;

	/* What the preset has, as the device last said */
	QVariantMap m_state;
	QVariantMap m_recall;
	/* One row per captured value: whether it is recalled, and its editor */
	struct Row {
		QCheckBox *recall;
		QWidget *editor;
	};
	QMap<QString, Row> m_rows;
};
