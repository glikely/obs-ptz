/* Pan Tilt Zoom camera controls
 *
 * Copyright 2020 Grant Likely <grant.likely@secretlab.ca>
 *
 * SPDX-License-Identifier: GPLv2
 */
#pragma once

#include <QTimer>
#include <obs.hpp>
#include <QDockWidget>
#include <QtGamepad/QGamepad>

#include "ptz-device.hpp"
#include "ui_ptz-controls.h"

#define STRINGIFY(x) #x
#define TOSTRING(x) STRINGIFY(x)

class PTZControls : public QDockWidget {
	Q_OBJECT

private:
	static void OBSFrontendEventWrapper(enum obs_frontend_event event, void *ptr);
	static PTZControls *instance;
	void OBSFrontendEvent(enum obs_frontend_event event);

	std::unique_ptr<Ui::PTZControls> ui;

	unsigned int current_cam;

	bool use_gamepad;
	QGamepad *gamepad;

	void SaveConfig();
	void LoadConfig();

	void full_stop();
	void setPanTilt(double pan, double tilt);

	void setCurrent(unsigned int index);

	PTZDevice * currCamera();

	obs_hotkey_id panTiltLeftHotkey = 0;
	obs_hotkey_id panTiltRightHotkey = 0;
	obs_hotkey_id panTiltUpHotkey = 0;
	obs_hotkey_id panTiltDownHotkey = 0;
	obs_hotkey_id zoomWideHotkey = 0;
	obs_hotkey_id zoomTeleHotkey = 0;

private slots:
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
	void on_panTiltGamepad();

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

	void on_targetButton_preview_clicked(bool checked);
	void on_targetButton_program_clicked(bool checked);

	void currentChanged(QModelIndex current, QModelIndex previous);

	void on_presetListView_activated(QModelIndex index);
	void on_presetListView_customContextMenuRequested(const QPoint &pos);
	void on_configButton_released();

public:
	PTZControls(QWidget *parent = nullptr);
	~PTZControls();
	void setGamepadEnabled(bool enable);
	bool gamepadEnabled() { return use_gamepad; };
	static PTZControls* getInstance() { return instance; };
};
