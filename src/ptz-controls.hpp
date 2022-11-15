/* Pan Tilt Zoom camera controls
 *
 * Copyright 2020 Grant Likely <grant.likely@secretlab.ca>
 *
 * SPDX-License-Identifier: GPLv2
 */
#pragma once

#include "ptz.h"
#include <QTimer>
#include <obs.hpp>
#include <QDockWidget>
#include "ptz-device.hpp"
#include "ui_ptz-controls.h"

#ifdef OBS_PTZ_GAMEPAD
class PTZGamePad;
class Ui_PTZSettings;
#endif // #ifdef OBS_PTZ_GAMEPAD

class PTZControls : public QDockWidget {
	Q_OBJECT

private:
	static void OBSFrontendEventWrapper(enum obs_frontend_event event,
					    void *ptr);
	static PTZControls *instance;
	void OBSFrontendEvent(enum obs_frontend_event event);

	std::unique_ptr<Ui::PTZControls> ui;

	bool live_moves_disabled = false;

	// Current status
	bool pantiltingFlag = false;
	bool zoomingFlag = false;
	bool focusingFlag = false;

#ifdef OBS_PTZ_GAMEPAD
	// Gamepad
	bool useGamepad = false;
	PTZGamePad *gamepad = nullptr;
#endif // #ifdef OBS_PTZ_GAMEPAD

	void copyActionsDynamicProperties();
	void SaveConfig();
	void LoadConfig();

	void setFocus(double speed);

	void setCurrent(unsigned int index);
	void presetRecall(int id);
	void setAutofocusEnabled(bool autofocus_on);

	PTZDevice *currCamera();

	QList<obs_hotkey_id> hotkeys;
	QMap<obs_hotkey_id, int> preset_hotkey_map;

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

	void on_actionFollowPreview_toggled(bool checked);
	void on_actionFollowProgram_toggled(bool checked);

	void currentChanged(QModelIndex current, QModelIndex previous);
	void settingsChanged(OBSData settings);
	void updateMoveControls();

	void on_presetListView_activated(QModelIndex index);
	void on_presetListView_customContextMenuRequested(const QPoint &pos);
	void on_cameraList_doubleClicked(const QModelIndex &index);
	void on_cameraList_customContextMenuRequested(const QPoint &pos);
	void on_actionPTZProperties_triggered();
	void on_actionDisableLiveMoves_toggled(bool checked);

public:
	PTZControls(QWidget *parent = nullptr);
	~PTZControls();
	void setDisableLiveMoves(bool enable);
	bool liveMovesDisabled() { return live_moves_disabled; };
	static PTZControls *getInstance() { return instance; };

	void setPanTilt(double pan, double tilt);
	void setZoom(double speed);
#ifdef OBS_PTZ_GAMEPAD
	void nextCamera();
	void prevCamera();
	void nextPreset();
	void prevPreset();
	void setPreset(int index);
	void activeSelectedPreset();
	void changeSpeed(int amount);

	bool gamepadEnabled() { return useGamepad; };
	void setGamepadEnabled(bool enable, Ui_PTZSettings *ui);
#endif // #ifdef OBS_PTZ_GAMEPAD
};
