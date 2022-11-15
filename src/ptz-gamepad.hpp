/* PTZ Gamepad Windows Support
* Copyright 2022 Eric Schmidt <ericbschmidt@gmail.com>
* SPDX-License-Identifier: GPLv2
*/

#pragma once

#include <QObject>

enum PTZGamepadStatus : char {
	GAMEPAD_STATUS_BLANK = 0,
	GAMEPAD_STATUS_NONE,
	GAMEPAD_STATUS_FOUND,
	GAMEPAD_STATUS_NOT_SUPPORTED,
};

enum PTZGamepadButton : char {
	GAMEPAD_DPAD_UP = 0,
	GAMEPAD_DPAD_DOWN,
	GAMEPAD_DPAD_LEFT,
	GAMEPAD_DPAD_RIGHT,
	GAMEPAD_LEFT_SHOULDER,
	GAMEPAD_RIGHT_SHOULDER,
	GAMEPAD_START,
	GAMEPAD_A,
	GAMEPAD_B,
	GAMEPAD_X,
	GAMEPAD_Y,
	GAMEPAD_RIGHT_THUMB,
	GAMEPAD_LEFT_THUMB,
	GAMEPAD_BACK,
	GAMEPAD_BUTTON_COUNT,
};

class PTZGamePadBase : public QObject {
	Q_OBJECT

protected:
	PTZGamepadStatus status;
	int gamepadID;
	double rightStickX;
	double rightStickY;
	double leftStickX;
	double leftStickY;
	unsigned short buttonsDown;

public:
	virtual bool isGamepadSupportEnabled() = 0;
	virtual void setGamepadEnabled(bool enabled) = 0;
	virtual PTZGamepadStatus getGamepadStatus() = 0;
	static QString getStatusString(char status)
	{
		switch (status) {
		case PTZGamepadStatus::GAMEPAD_STATUS_NOT_SUPPORTED:
			return QStringLiteral("Not Supported");
		case PTZGamepadStatus::GAMEPAD_STATUS_FOUND:
			return QStringLiteral("Gamepad Found");
		case PTZGamepadStatus::GAMEPAD_STATUS_NONE:
			return QStringLiteral("No Gamepad Found");
		case PTZGamepadStatus::GAMEPAD_STATUS_BLANK:
		default:
			return QStringLiteral("");
		}
	}

signals:
	void rightAxisChanged(float stickX, float stickY);
	void leftAxisChanged(float stickX, float stickY);
	void buttonDownChanged(PTZGamepadButton button);
	void uiGamepadStatusChanged(char status);
};