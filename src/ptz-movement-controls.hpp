/* Pan Tilt Zoom camera movement controls
 *
 * Copyright 2020-2026 Grant Likely <grant.likely@secretlab.ca>
 *
 * SPDX-License-Identifier: GPLv2
 */
#pragma once

#include <memory>
#include <QWidget>
#include <QTimer>
#include <QPersistentModelIndex>

namespace Ui {
class PTZMovementControls;
}
class QAbstractButton;

/* The buttons and touch pad that move a camera: pan and tilt, home, zoom and
 * focus. It moves whichever device it has been given (setDevice()), so the
 * dock and the settings dialog can each have one, following their own
 * selection, and all the movement logic is in one place: the speed that
 * the modifier keys pick, ramping a held button up, and stopping a camera
 * that was moving when the selection moved away.
 *
 * What it enables follows what the device can do and, in studio mode, whether
 * moving it is locked (updateControls()), which it redoes by itself when the
 * device or the dock's settings change. */
class PTZMovementControls : public QWidget {
	Q_OBJECT

public:
	explicit PTZMovementControls(QWidget *parent = nullptr);
	~PTZMovementControls() override;

	/* The device to move. Stops the previous one, if it was moving */
	void setDevice(const QModelIndex &index);
	QModelIndex device() const { return QModelIndex(m_device); }

	/* Enable only what the device can do, and what is not locked */
	void updateControls();
	/* The icons come in a light and a dark variant */
	void refreshTheme();

	/* Move at a speed from -1 to 1, as the buttons, the touch pad and a
	 * joystick do. Zero stops. */
	void setPanTilt(double pan, double tilt, double pan_accel = 0, double tilt_accel = 0);
	void setZoom(double zoom);
	void setFocus(double focus);

	/* Turn the autofocus on or off, as the button does: ask the camera,
	 * and follow the button */
	void requestAutofocus(bool on);
	bool autofocusOn() const;

	/* Size the buttons by the theme, the way the dock sizes its rows, and
	 * keep them that size, instead of stretching them to fill whatever room
	 * there is, as the dock's do. The controls are then a fixed size, which
	 * follows the theme's density. */
	void setThemeSized(bool themeSized);

	/* Whether pan and tilt are the touch pad rather than the buttons */
	bool onscreenJoystick() const;
	void setOnscreenJoystick(bool enabled);

private:
	std::unique_ptr<Ui::PTZMovementControls> ui;
	QPersistentModelIndex m_device;

	double pan_speed = 0.0;
	double pan_accel = 0.0;
	double tilt_speed = 0.0;
	double tilt_accel = 0.0;
	double zoom_speed = 0.0;
	double zoom_accel = 0.0;
	double focus_speed = 0.0;
	double focus_accel = 0.0;
	QTimer accel_timer;
	/* Keeps the buttons square as they stretch, unless they are theme sized */
	QObject *m_squareFilter = nullptr;
	bool m_themeSized = false;
	/* The grid's margins as the .ui has them, which theme sizing takes off */
	QMargins m_margins;

	bool pantiltingFlag = false;
	bool zoomingFlag = false;
	bool focusingFlag = false;

	bool callDevice(const char *method, calldata_t *cd = nullptr) const;
	bool callDevice(const char *method, const char *arg, double val) const;
	void keypressPanTilt(double pan, double tilt);
	void showAutofocus(bool on);
	void updateFocusControls();
	void stop();
	void applySize();

private slots:
	void accelTimerHandler();
	void onHomeButtonContextMenu(const QPoint &pos);
	void on_pantiltStack_customContextMenuRequested(const QPoint &pos);
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
};
