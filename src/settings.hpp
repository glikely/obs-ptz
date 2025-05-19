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
#include <QMenu>
#include <properties-view.hpp>
#if defined(ENABLE_JOYSTICK)
#include <QJoysticks.h>
#endif

class Ui_PTZSettings;

#if defined(ENABLE_JOYSTICK)
class PTZJoyButtonMapper : public QPushButton {
	Q_OBJECT;

public:
	PTZJoyButtonMapper(QWidget *parent, size_t button);

public slots:
	void on_joystickButtonMapping_triggered();
	void on_joystickButtonMappingChanged(size_t button, ptz_joy_action_id action);
	void on_joystickButtonEvent(const QJoystickButtonEvent);

protected:
	size_t button;
};
#endif

class SourceNameDelegate : public QStyledItemDelegate {
	Q_OBJECT

public:
	SourceNameDelegate(QObject *parent = nullptr) : QStyledItemDelegate(parent){};
	virtual QString displayText(const QVariant &value, const QLocale &locale) const;
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

/* Joystick Support */
#if defined(ENABLE_JOYSTICK)
protected:
	void joystickSetup();
	QStringListModel m_joystickNamesModel;
	QList<QLabel *> joystickAxisLabels;
	QList<QComboBox *> joystickAxisCBs;
	QList<QPushButton *> joystickButtonButtons;
protected slots:
	void on_joystickGroupBox_toggled(bool checked);
	void on_joystickSpeedSlider_doubleValChanged(double val);
	void on_joystickDeadzoneSlider_doubleValChanged(double val);
	void on_joystickAxisActionChanged(int idx);
	void on_joystickButtonActionChanged(int idx);
	void joystickUpdate();
	void joystickAxisMappingChanged(size_t axis, ptz_joy_action_id action);
	void joystickCurrentChanged(QModelIndex, QModelIndex);
	void joystickAxisEvent(const QJoystickAxisEvent);
#else  /* ENABLE_JOYSTICK */
protected:
	void joystickSetup();
#endif /* ENABLE_JOYSTICK */

public slots:
	void on_addPTZ_clicked();
	void on_removePTZ_clicked();
	void on_applyButton_clicked();

	void currentChanged(const QModelIndex &current, const QModelIndex &previous);
	void settingsChanged(OBSData settings);
	obs_properties_t *getProperties(void);
	void updateProperties(OBSData old_settings, OBSData new_settings);
	void showDevice(uint32_t device_id);
};

void ptz_settings_show(uint32_t device_id = 0);
extern "C" void ptz_init_settings();
