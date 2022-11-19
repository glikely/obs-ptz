/* Pan Tilt Zoom settings window
 *
 * Copyright 2020 Grant Likely <grant.likely@secretlab.ca>
 *
 * SPDX-License-Identifier: GPLv2
 */
#pragma once

#include <QWidget>
#include <QStyledItemDelegate>
#include <QString>
#include <imported/properties-view.hpp>

class Ui_PTZSettings;

class SourceNameDelegate : public QStyledItemDelegate {
	Q_OBJECT

public:
	SourceNameDelegate(QObject *parent = nullptr)
		: QStyledItemDelegate(parent){};
	QWidget *createEditor(QWidget *parent,
			      const QStyleOptionViewItem &option,
			      const QModelIndex &index) const override;
	void setEditorData(QWidget *editor,
			   const QModelIndex &index) const override;
	void setModelData(QWidget *editor, QAbstractItemModel *model,
			  const QModelIndex &index) const override;
};

class PTZSettings : public QWidget {
	Q_OBJECT

private:
	Ui_PTZSettings *ui;
	OBSData settings;
	OBSPropertiesView *propertiesView = nullptr;
	void current_device_changed();

public:
	PTZSettings();
	~PTZSettings();
	void set_selected(uint32_t device_id);

public slots:
	void on_close_clicked();

	void on_addPTZ_clicked();
	void on_removePTZ_clicked();
	void on_applyButton_clicked();
	void on_livemoveCheckBox_stateChanged(int state);
	void on_enableDebugLogCheckBox_stateChanged(int state);
	void on_gamepadCheckBox_stateChanged(int state);

	void currentChanged(const QModelIndex &current,
			    const QModelIndex &previous);
	obs_properties_t *getProperties(void);
	void updateProperties(OBSData old_settings, OBSData new_settings);

protected slots:
	void uiGamepadStatus(char status);
};

void ptz_settings_show(uint32_t device_id = 0);
extern "C" void ptz_init_settings();
