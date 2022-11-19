/* PTZ Gamepad Windows Support
* Copyright 2022 Eric Schmidt <ericbschmidt@gmail.com>
* SPDX-License-Identifier: GPLv2
*/

#pragma once

#include <iostream>
#include <Windows.h>
#include <Xinput.h>
#include <QObject>
#include <qthread.h>
#include "ptz-gamepad.hpp"

class PTZGamepadThread;

static constexpr int NUM_FRAMES_CONTROLLER_CHECK = 60;
static constexpr float DEADZONE_X = 0.08f;
static constexpr float DEADZONE_Y = 0.08f;

static_assert(DEADZONE_X < 1.0f && DEADZONE_X >= 0.0f);
static_assert(DEADZONE_Y < 1.0f && DEADZONE_Y >= 0.0f);

class PTZGamePad : public PTZGamePadBase
{
private:
	PTZGamepadThread *thread;
	XINPUT_STATE state;
	unsigned long lastPacketNumber;
	int frameCount;
	void resetButtons();
	void initializeGamepads();
	void inputHandleRightStick();
	void inputHandleLeftStick();
	void inputHandleButtons();
	void convertStickInput(short thumb, double deadzone, double *outStick);

public:
	PTZGamePad();
	~PTZGamePad();
	bool isGamepadSupportEnabled() { return true; };
	void setGamepadEnabled(bool enabled);
	PTZGamepadStatus getGamepadStatus() { return status; };
	void gamepadTick();
	void startThread();
	void stopThread();
};

class PTZGamepadThread : public QThread {
private:
	PTZGamePad *gamepad;
	QAtomicInt stop;

public:
	PTZGamepadThread(PTZGamePad *gamepadInput);
	void run() override;
	void reset() { stop.fetchAndStoreAcquire(0); };
	void kill() { stop.fetchAndStoreAcquire(1); };
};
