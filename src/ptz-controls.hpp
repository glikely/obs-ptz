/* Pan Tilt Zoom camera controls
 *
 * Copyright 2020 Grant Likely <grant.likely@secretlab.ca>
 *
 * SPDX-License-Identifier: GPLv2
 */
#pragma once

#include "ptz.h"
#include <QTimer>
#include <QStyledItemDelegate>
#include <obs.hpp>
#if defined(ENABLE_JOYSTICK)
#include <QJoysticks.h>
#endif
#include "touch-control.hpp"
#include "ptz-device.hpp"
#include "ui_ptz-controls.h"

typedef size_t ptz_joy_action_id;
enum ptz_joy_action {
	PTZ_JOY_ACTION_NONE = 0,
	PTZ_JOY_ACTION_PAN,
	PTZ_JOY_ACTION_PAN_INVERT,
	PTZ_JOY_ACTION_TILT,
	PTZ_JOY_ACTION_TILT_INVERT,
	PTZ_JOY_ACTION_ZOOM,
	PTZ_JOY_ACTION_ZOOM_INVERT,
	PTZ_JOY_ACTION_FOCUS,
	PTZ_JOY_ACTION_FOCUS_INVERT,
	PTZ_JOY_ACTION_LAST_VALUE
};
typedef enum ptz_joy_action ptz_joy_action_t;
extern const char *ptz_joy_action_axis_names[PTZ_JOY_ACTION_LAST_VALUE];

class PTZControls : public QFrame {
	Q_OBJECT

private:
	static PTZControls *instance;
	static void onFrontendEvent(enum obs_frontend_event event, void *ptr);
	void handleFrontendEvent(enum obs_frontend_event event);

	std::unique_ptr<Ui::PTZControls> ui;
	TouchControl *pantilt_widget;

	bool live_move_lock_enabled = true;
	bool autoselect_enabled = false;
	bool speed_ramp_enabled = false;

	// Current status
	double pan_speed = 0.0;
	double pan_accel = 0.0;
	double tilt_speed = 0.0;
	double tilt_accel = 0.0;
	double zoom_speed = 0.0;
	double zoom_accel = 0.0;
	double focus_speed = 0.0;
	double focus_accel = 0.0;
	QTimer accel_timer;

	bool pantiltingFlag = false;
	bool zoomingFlag = false;
	bool focusingFlag = false;

	void copyActionsDynamicProperties();
	void SaveConfig();
	void LoadConfig();

	void setZoom(double speed);
	void setFocus(double speed);

	void setCurrent(unsigned int index);
	int presetIndexToId(QModelIndex index);
	void presetSet(long long id);
	void presetRecall(long long id);
	void presetReset(long long id);
	void setAutofocusEnabled(bool autofocus_on);

	bool callCurrentDevice(const char *method, calldata_t *cd = nullptr) const;
	bool callCurrentDevice(const char *method, const char *arg, long long val) const;
	bool callCurrentDevice(const char *method, const char *arg, double val) const;
	bool callCurrentDevice(const char *method, const char *arg, bool val) const;

	QList<obs_hotkey_id> hotkeys;
	QMap<obs_hotkey_id, int> preset_hotkey_map;

public slots:
	void autoselectDevice(OBSSource scene);
private slots:
	void updateMoveControls();
	void onHomeButtonContextMenu(const QPoint &pos);
	void setPanTilt(double pan, double tilt, double pan_accel = 0, double tilt_accel = 0);
	void keypressPanTilt(double pan, double tilt);
	void on_panTiltButton_up_pressed();
	void on_panTiltButton_up_released();
	void on_panTiltButton_upleft_pressed();
	void on_panTiltButton_upleft_released();
	void on_panTiltButton_upright_pressed();
	void on_panTiltButton_upright_released();
	void on_panTiltButton_left_pressed();
	void on_panTiltButton_left_released();
	void on_panTiltButton_right_pressed();
	void on_panTiltButton_right_released();
	void on_panTiltButton_down_pressed();
	void on_panTiltButton_down_released();
	void on_panTiltButton_downleft_pressed();
	void on_panTiltButton_downleft_released();
	void on_panTiltButton_downright_pressed();
	void on_panTiltButton_downright_released();
	void on_panTiltButton_home_released();

	void on_zoomButton_tele_pressed();
	void on_zoomButton_tele_released();
	void on_zoomButton_wide_pressed();
	void on_zoomButton_wide_released();

	void on_focusButton_auto_clicked(bool checked);
	void on_focusButton_near_pressed();
	void on_focusButton_near_released();
	void on_focusButton_far_pressed();
	void on_focusButton_far_released();
	void on_focusButton_onetouch_clicked();

	void currentChanged(QModelIndex current, QModelIndex previous);
	void settingsChanged(const QModelIndex &topleft, const QModelIndex &bottomRight);

	void presetUpdateActions();
	void on_presetListView_activated(QModelIndex index);
	void on_pantiltStack_customContextMenuRequested(const QPoint &pos);
	void on_presetListView_customContextMenuRequested(const QPoint &pos);
	void on_cameraList_customContextMenuRequested(const QPoint &pos);
	void on_actionProperties_triggered();
	void on_actionPresetAdd_triggered();
	void on_actionPresetRemove_triggered();
	void on_actionPresetMoveUp_triggered();
	void on_actionPresetMoveDown_triggered();
	void on_actionPresetSave_triggered();
	void on_actionPresetClear_triggered();
	void on_actionPresetRename_triggered();

	void accelTimerHandler();

	/* Joystick support */
protected:
	bool m_joystick_enable = false;
	int m_joystick_id = -1;
	double m_joystick_deadzone = 0.0;
	double m_joystick_speed = 1.0;
	int joystick_pan_axis = -1;
	bool joystick_pan_invert = false;
	int joystick_tilt_axis = -1;
	bool joystick_tilt_invert = false;
	int joystick_zoom_axis = -1;
	bool joystick_zoom_invert = false;
	int joystick_focus_axis = -1;
	bool joystick_focus_invert = false;
	QMap<size_t, ptz_joy_action_id> joystick_axis_actions;
	QMap<size_t, QString> joystick_button_hotkey_mappings;

public slots:
	void setJoystickAxisAction(size_t axis, ptz_joy_action_id);
	void setJoystickButtonHotkey(size_t button, QString);

signals:
	void joystickAxisActionChanged(size_t axis, ptz_joy_action_id action);
	void joystickButtonHotkeyChanged(size_t button, QString hotkey);

#if defined(ENABLE_JOYSTICK)
public:
	void joystickSetup();
	bool joystickEnabled() { return m_joystick_enable; };
	double joystickDeadzone() { return m_joystick_deadzone; };
	double joystickSpeed() { return m_joystick_speed; };
	void setJoystickEnabled(bool enable);
	void setJoystickSpeed(double speed);
	void setJoystickDeadzone(double deadzone);
	int joystickId() { return m_joystick_id; };
	void setJoystickId(int id) { m_joystick_id = id; };
	double readAxis(const QJoystickDevice *jd, int axis, bool invert);
	ptz_joy_action_id joystickAxisAction(size_t axis) { return joystick_axis_actions[axis]; };
	QString joystickButtonHotkey(size_t button) { return joystick_button_hotkey_mappings[button]; };

protected slots:
	void joystickAxesChanged(const QJoystickDevice *jd, uint32_t updated);
	void joystickAxisEvent(const QJoystickAxisEvent evt);
	void joystickButtonEvent(const QJoystickButtonEvent evt);
	void joystickPOVEvent(const QJoystickPOVEvent evt);
#else
public:
	void joystickSetup() {};
#endif /* ENABLE_JOYSTICK */

public:
	PTZControls(QWidget *parent = nullptr);
	bool autoselectEnabled() { return autoselect_enabled; };
	bool liveMoveLockEnabled() { return live_move_lock_enabled; };
	bool liveMoveLockActive() { return live_move_lock_enabled && obs_frontend_preview_program_mode_active(); };
	bool speedRampEnabled() { return speed_ramp_enabled; };
	static PTZControls *getInstance() { return instance; };

public slots:
	void setAutoselectEnabled(bool enable);
	void setLiveMoveLockEnabled(bool enable);
	void setSpeedRampEnabled(bool enable);

signals:
	void autoselectEnabledChanged(bool enabled);
	void liveMoveLockEnabledChanged(bool enabled);
	void speedRampEnabledChanged(bool enabled);
};

class PTZDeviceListDelegate : public QStyledItemDelegate {
	Q_OBJECT

public:
	struct CellLayout {
		QRect text;
		QRect status;
		QRect lock;
	};

	PTZDeviceListDelegate(QObject *parent);
	virtual QSize sizeHint(const QStyleOptionViewItem &option, const QModelIndex &index) const override;
	virtual void paint(QPainter *painter, const QStyleOptionViewItem &option,
			   const QModelIndex &index) const override;
	virtual bool editorEvent(QEvent *event, QAbstractItemModel *model, const QStyleOptionViewItem &option,
				 const QModelIndex &index) override;
	virtual bool helpEvent(QHelpEvent *event, QAbstractItemView *view, const QStyleOptionViewItem &option,
			       const QModelIndex &index) override;

private:
	CellLayout layoutCell(const QModelIndex &index, const QStyleOptionViewItem &option) const;

	QIcon lockedIcon;
	QIcon unlockedIcon;
	QIcon disconnectedIcon;
};
