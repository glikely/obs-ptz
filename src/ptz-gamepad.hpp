/* PTZ Gamepad Windows Support
*/

#pragma once

#include <iostream>
#include <Windows.h>
#include <Xinput.h>
#include <QObject>
#include <qthread.h>

#ifdef OBS_PTZ_GAMEPAD
class Ui_PTZSettings;
class PTZControls;
class PTZGamepadThread;

static constexpr int NUM_FRAMES_CONTROLLER_CHECK = 120;
static constexpr int INVALID_GAMEPAD_ID = -1;
static constexpr float DEADZONE_X = 0.05f;
static constexpr float DEADZONE_Y = 0.02f;

static_assert(DEADZONE_X < 1.0f && DEADZONE_X >= 0.0f);
static_assert(DEADZONE_Y < 1.0f && DEADZONE_Y >= 0.0f);

class PTZGamePad : public QObject
{
	Q_OBJECT

private:
	PTZGamepadThread *thread;
	PTZControls *controls;
	Ui_PTZSettings *uiSettings;
	int gamepadID;
	XINPUT_STATE state;
	unsigned long lastPacketNumber;
	int frameCount;
	float rightStickX;
	float rightStickY;
	float leftStickY;
	unsigned short buttonsDown;
	void resetButtons();
	void initializeGamepads();
	void inputHandleRightStick();
	void inputHandleLeftStick();
	void inputHandleButtons();

public:
	PTZGamePad(PTZControls *ptzControls);
	~PTZGamePad();
	void setGamepadEnabled(bool enabled, Ui_PTZSettings *ui);
	void gamepadTick();
	void startThread();
	void stopThread();
	void cameraSelectUp(short actionData);
	void cameraSelectDown(short actionData);
	void cameraSpeedUp(short actionData);
	void cameraSpeedDown(short actionData);
	void presetSelectUp(short actionData);
	void presetSelectDown(short actionData);
	void presetSelectGo(short actionData);
	void presetSelectSave(short actionData);
	void presetButton(short actionData);

signals:
	void rightAxisChanged(float stickX, float stickY);
	void leftAxisChanged(float stickX);
	void buttonDownChanged(unsigned short buttons);
	void uiGamepadStatusChanged(byte status);

private slots:
	void setPanTilt(float stickX, float stickY);
	void setZoom(float stickY);
	void handleButton(unsigned short buttons);
	void uiGamepadStatus(byte status);
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
#endif // #ifdef OBS_PTZ_GAMEPAD
