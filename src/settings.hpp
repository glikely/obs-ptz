/* Pan Tilt Zoom settings window
 *
 * Copyright 2020 Grant Likely <grant.likely@secretlab.ca>
 *
 * SPDX-License-Identifier: GPLv2
 */
#pragma once

#include <QWidget>
#include <QTimer>
#include <QStyledItemDelegate>
#include <QString>
#include <QMenu>
#include <properties-view.hpp>
#if defined(ENABLE_JOYSTICK)
#include <QStringListModel>
#include <QPushButton>
#include <QJoysticks.h>
#endif

class Ui_PTZSettings;
class QLabel;
class QPushButton;

#if defined(ENABLE_JOYSTICK)
class PTZJoyButtonMapper : public QPushButton {
	Q_OBJECT;

public:
	PTZJoyButtonMapper(QWidget *parent, size_t button);

public slots:
	void on_menuAction();
	void on_hotkeyChanged(size_t button, QString hotkey_name);
	void on_joystickButtonEvent(const QJoystickButtonEvent);

protected:
	size_t button;
};
#endif

class PTZSettings : public QWidget {
	Q_OBJECT

private:
	Ui_PTZSettings *ui;
	/* A device is shown as a status header, always visible, over three
	 * tabs: its state, its settings, and its diagnostics. The state, what
	 * the camera reports and never persisted, is ui->stateView, a
	 * PTZStateView, which goes back as requests (see
	 * PTZDevice::requestState()). The settings are edited in a properties
	 * view over `settings`, which is what the device saves, and goes back
	 * through update() (in a filter's case, the filter's own settings). It
	 * has its own internal scrolling turned off
	 * (OBSPropertiesView::setScrolling(), which makes it size itself to its
	 * content instead) so the scroll area around it is what actually
	 * scrolls. */
	OBSData settings;
	OBSPropertiesView *propertiesView = nullptr;
	void updateHeader();
	void current_device_changed();
	uint32_t currentDeviceId() const;
	/* The device's statistics aren't told, they are read: every second
	 * while the dialog is showing, for the state view's diagnostics */
	QTimer statisticsTimer;
	void refreshStatistics();

public:
	PTZSettings();
	~PTZSettings();

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
	void addDevice();
	void on_removePTZ_clicked();
	void on_applyButton_clicked();

	void currentChanged(const QModelIndex &current, const QModelIndex &previous);
	void deviceSettingsUpdated(uint32_t device_id);
	void deviceStateUpdated(uint32_t device_id, OBSData changed);
	obs_properties_t *getProperties(void);
	void updateProperties(OBSData old_settings, OBSData new_settings);
	void showDevice(const QModelIndex &index);
};

void ptz_settings_show(const QModelIndex &index = QModelIndex());
extern "C" void ptz_init_settings();
