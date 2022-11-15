/* PTZ Gamepad Windows Support
* This class implements support for Xbox controllers using XInput. Enable in the settings.
* Default buttons/axis:
* left axis - zoom
* right axis - pan/tilt
* dpad up - camera select up
* dpad down - camera select down
* dpad left - camera speed down
* dpad right - camera speed up
* left shoulder - preset up
* right shoulder - preset down
* start - activate selected preset
* A - preset 1
* B - preset 2
* X - preset 3
* Y - preset 4
* Thumb L - preset 5
* Thumb R - preset 6
* back - preset 7
* 
* Known Issue:
* It can issue too many commands depending upon use which can overflow the visca over ip command buffer.
* This is not noticeable usually, but the command to stop the gamepad could be missed in rare cases.
* 
* Wishlist Improvements:
* Add a button mapping configuration.
* Add a modifier button to add more presets.
* Add focus controls as an option.
* Support multiple gamepads.
* Support different types of gamepads. (DirectInput)
* Add support for linux and mac.
*/

#include "ptz-controls.hpp"
#include "ui_settings.h"
#include "ptz-device.hpp"
#include "ptz-gamepad.hpp"

#ifdef OBS_PTZ_GAMEPAD

#define GAMEPAD_POLL_MS			5
#define GAMEPAD_SPEED_CHANGE	5

#define GAMEPAD_STATUS_BLANK	0
#define GAMEPAD_STATUS_NONE		1
#define GAMEPAD_STATUS_FOUND	2

enum PTZGamepadAction:byte 
{
	CAMERA_SELECT_UP = 0,
	CAMERA_SELECT_DOWN,
	CAMERA_SPEED_UP,
	CAMERA_SPEED_DOWN,
	PRESET_SELECT_UP,
	PRESET_SELECT_DOWN,
	PRESET_SELECT_GO,
	PRESET_BUTTON,
	PTZ_ACTIONS_MAX,
};

typedef void (PTZGamePad::*gamepad_action_t)(short actionData);
gamepad_action_t gamepadActions[] = 
{
	&PTZGamePad::cameraSelectUp,
	&PTZGamePad::cameraSelectDown,
	&PTZGamePad::cameraSpeedUp,
	&PTZGamePad::cameraSpeedDown,
	&PTZGamePad::presetSelectUp,
	&PTZGamePad::presetSelectDown,
	&PTZGamePad::presetSelectGo,
	&PTZGamePad::presetButton,
};

static_assert((sizeof(gamepadActions) / sizeof(gamepad_action_t)) == PTZGamepadAction::PTZ_ACTIONS_MAX, "PTZGamepadAction needs to match gamepadActions");

struct ButtonMapping
{
	unsigned short xinputButton;
	PTZGamepadAction action;
	short actionData;
};

ButtonMapping buttonMap[] =
{
	{XINPUT_GAMEPAD_DPAD_UP, PTZGamepadAction::CAMERA_SELECT_UP, 0},
	{XINPUT_GAMEPAD_DPAD_DOWN, PTZGamepadAction::CAMERA_SELECT_DOWN, 0},
	{XINPUT_GAMEPAD_DPAD_LEFT, PTZGamepadAction::CAMERA_SPEED_DOWN, 0},
	{XINPUT_GAMEPAD_DPAD_RIGHT, PTZGamepadAction::CAMERA_SPEED_UP, 0},
	{XINPUT_GAMEPAD_LEFT_SHOULDER, PTZGamepadAction::PRESET_SELECT_UP, 0},
	{XINPUT_GAMEPAD_RIGHT_SHOULDER, PTZGamepadAction::PRESET_SELECT_DOWN, 0},
	{XINPUT_GAMEPAD_START, PTZGamepadAction::PRESET_SELECT_GO, 0},
	{XINPUT_GAMEPAD_A, PTZGamepadAction::PRESET_BUTTON, 0},
	{XINPUT_GAMEPAD_B, PTZGamepadAction::PRESET_BUTTON, 1},
	{XINPUT_GAMEPAD_X, PTZGamepadAction::PRESET_BUTTON, 2},
	{XINPUT_GAMEPAD_Y, PTZGamepadAction::PRESET_BUTTON, 3},
	{XINPUT_GAMEPAD_LEFT_THUMB, PTZGamepadAction::PRESET_BUTTON, 4},
	{XINPUT_GAMEPAD_RIGHT_THUMB, PTZGamepadAction::PRESET_BUTTON, 5},
	{XINPUT_GAMEPAD_BACK, PTZGamepadAction::PRESET_BUTTON, 6},
};

#define BUTTONMAP_COUNT		sizeof(buttonMap) / sizeof(ButtonMapping)

PTZGamepadThread::PTZGamepadThread(PTZGamePad* gamepadInput)
{
	gamepad = gamepadInput;
	stop = false;
}

void PTZGamepadThread::run() 
{
	while (!stop.testAndSetAcquire(1, 0))
	{
		gamepad->gamepadTick();
		Sleep(GAMEPAD_POLL_MS);
	}
}

PTZGamePad::PTZGamePad(PTZControls *ptzControls)
{
	controls = ptzControls;
	uiSettings = nullptr;
	gamepadID = INVALID_GAMEPAD_ID;
	ZeroMemory(&state, sizeof(XINPUT_STATE));
	lastPacketNumber = 0;
	frameCount = 0;
	resetButtons();

	connect(this, &PTZGamePad::rightAxisChanged, this, &PTZGamePad::setPanTilt);
	connect(this, &PTZGamePad::leftAxisChanged, this, &PTZGamePad::setZoom);
	connect(this, &PTZGamePad::buttonDownChanged, this, &PTZGamePad::handleButton);
	connect(this, &PTZGamePad::uiGamepadStatusChanged, this, &PTZGamePad::uiGamepadStatus);

	thread = new PTZGamepadThread(this);
}

PTZGamePad::~PTZGamePad()
{
	delete thread;
}

void PTZGamePad::resetButtons()
{
	rightStickX = 0.0f;
	rightStickY = 0.0f;
	leftStickY = 0.0f;
	buttonsDown = 0;
}

void PTZGamePad::startThread()
{
	thread->reset();
	thread->start();
}

void PTZGamePad::stopThread() 
{
	thread->kill();
	thread->wait();
}

void PTZGamePad::gamepadTick()
{
	// try to reconnect to the controller
	if (gamepadID == INVALID_GAMEPAD_ID)
	{
		frameCount++;
		if (frameCount < NUM_FRAMES_CONTROLLER_CHECK)
			return;

		initializeGamepads();
		frameCount = 0;
		if (gamepadID == INVALID_GAMEPAD_ID)
		{
			emit uiGamepadStatusChanged(GAMEPAD_STATUS_NONE);
			return;
		}
		else
		{
			emit uiGamepadStatusChanged(GAMEPAD_STATUS_FOUND);
		}
	}

	ZeroMemory(&state, sizeof(XINPUT_STATE));

	const DWORD dwResult = XInputGetState(gamepadID, &state);

	// check for controller disconnect
	if (dwResult != ERROR_SUCCESS)
	{
		gamepadID = INVALID_GAMEPAD_ID;
		resetButtons();
		emit rightAxisChanged(rightStickX, rightStickY);
		emit leftAxisChanged(leftStickY);
		return;
	}

	// check if the controller has changed
	if (state.dwPacketNumber == lastPacketNumber)
		return;

	lastPacketNumber = state.dwPacketNumber;

	inputHandleRightStick();
	inputHandleLeftStick();
	inputHandleButtons();
}

void PTZGamePad::inputHandleRightStick()
{
	const float lastRightStickX = rightStickX;
	const float lastRightStickY = rightStickY;

	const float normRX = fmaxf(-1, (float)state.Gamepad.sThumbRX / 32767);
	const float normRY = fmaxf(-1, (float)state.Gamepad.sThumbRY / 32767);

	rightStickX = (abs(normRX) < DEADZONE_X ? 0 : (abs(normRX) - DEADZONE_X) * (normRX / abs(normRX)));
	rightStickY = (abs(normRY) < DEADZONE_Y ? 0 : (abs(normRY) - DEADZONE_Y) * (normRY / abs(normRY)));

	if constexpr (DEADZONE_X > 0.0f && DEADZONE_X < 1.0f)
		rightStickX *= 1 / (1 - DEADZONE_X);
	if constexpr (DEADZONE_Y > 0.0f && DEADZONE_Y < 1.0f)
		rightStickY *= 1 / (1 - DEADZONE_Y);

	if (lastRightStickX != rightStickX || lastRightStickY != rightStickY)
	{
		ptz_debug("PTZGamepad: right stick changed x=%.3f, y=%.3f", rightStickX, rightStickY);
		emit rightAxisChanged(rightStickX, rightStickY);
	}
}

void PTZGamePad::inputHandleLeftStick()
{
	const float lastLeftStickY = leftStickY;

	const float normLY = fmaxf(-1, (float)state.Gamepad.sThumbLY / 32767);

	leftStickY = (abs(normLY) < DEADZONE_Y ? 0 : (abs(normLY) - DEADZONE_Y) * (normLY / abs(normLY)));

	if constexpr (DEADZONE_Y > 0.0f && DEADZONE_Y < 1.0f)
		leftStickY *= 1 / (1 - DEADZONE_Y);

	if (lastLeftStickY != leftStickY)
	{
		ptz_debug("PTZGamepad: left stick changed x=%.3f", leftStickY);
		emit leftAxisChanged(leftStickY);
	}
}

void PTZGamePad::inputHandleButtons()
{
	unsigned short buttons = 0;

	for (int bIndex = 0; bIndex < BUTTONMAP_COUNT; ++bIndex)
	{
		const ButtonMapping* button = &buttonMap[bIndex];
		const bool buttonPressedLast = ((buttonsDown & button->xinputButton) != 0);
		const bool buttonPressedNow = ((state.Gamepad.wButtons & button->xinputButton) != 0);
		if (!buttonPressedLast && buttonPressedNow)
		{
			buttons |= button->xinputButton;
		}
	}

	buttonsDown = state.Gamepad.wButtons;

	if (buttons != 0)
	{
		emit buttonDownChanged(buttons);
	}
}

void PTZGamePad::uiGamepadStatus(byte status)
{
	if (!uiSettings)
	{
		return;
	}
	else if (status == GAMEPAD_STATUS_BLANK)
	{
		uiSettings->gamepadActiveText->setText(QStringLiteral(""));
	}
	else if (status == GAMEPAD_STATUS_NONE)
	{
		uiSettings->gamepadActiveText->setText(QStringLiteral("No Gamepad Detected"));
	}
	else if (status == GAMEPAD_STATUS_FOUND)
	{
		uiSettings->gamepadActiveText->setText(QStringLiteral("Found Gamepad"));
	}
}

void PTZGamePad::setGamepadEnabled(bool enabled, Ui_PTZSettings *ui)
{
	if (ui)
	{
		uiSettings = ui;
	}

	if (enabled) 
	{
		initializeGamepads();
		if (gamepadID != INVALID_GAMEPAD_ID) 
		{
			emit uiGamepadStatusChanged(GAMEPAD_STATUS_FOUND);
			startThread();
		}
		else
		{
			emit uiGamepadStatusChanged(GAMEPAD_STATUS_NONE);
			stopThread();
		}
	} 
	else 
	{
		gamepadID = INVALID_GAMEPAD_ID;
		emit uiGamepadStatusChanged(GAMEPAD_STATUS_BLANK);
		stopThread();
	}
}

void PTZGamePad::initializeGamepads()
{
	gamepadID = INVALID_GAMEPAD_ID;
	for (DWORD i = 0; i < XUSER_MAX_COUNT; i++) 
	{
		ZeroMemory(&state, sizeof(XINPUT_STATE));

		const DWORD dwResult = XInputGetState(i, &state);

		if (dwResult == ERROR_SUCCESS) {

			gamepadID = i;
			break;
		}
	}
}

void PTZGamePad::setPanTilt(float stickX, float stickY)
{
	controls->setPanTilt(stickX, stickY);
}

void PTZGamePad::setZoom(float stickY)
{
	controls->setZoom(stickY);
}

void PTZGamePad::handleButton(unsigned short buttons)
{
	for (int bIndex = 0; bIndex < BUTTONMAP_COUNT; ++bIndex)
	{
		const ButtonMapping* button = &buttonMap[bIndex];
		const bool buttonPressed = ((buttons & button->xinputButton) != 0);
		if (buttonPressed)
		{
			gamepad_action_t actionFunc = gamepadActions[button->action];
			std::invoke(actionFunc, this, button->actionData);
		}
	}
}

void PTZGamePad::cameraSelectUp(short actionData)
{
	Q_UNUSED(actionData);
	ptz_debug( "Gamepad button pressed: cameraSelectUp" );
	controls->prevCamera();
}

void PTZGamePad::cameraSelectDown(short actionData)
{
	Q_UNUSED(actionData);
	ptz_debug("Gamepad button pressed: cameraSelectDown");
	controls->nextCamera();
}

void PTZGamePad::cameraSpeedUp(short actionData)
{
	Q_UNUSED(actionData);
	ptz_debug("Gamepad button pressed: cameraSpeedUp");
	controls->changeSpeed(GAMEPAD_SPEED_CHANGE);
}

void PTZGamePad::cameraSpeedDown(short actionData)
{
	Q_UNUSED(actionData);
	ptz_debug("Gamepad button pressed: cameraSpeedDown");
	controls->changeSpeed(-1 * GAMEPAD_SPEED_CHANGE);
}

void PTZGamePad::presetSelectUp(short actionData)
{
	Q_UNUSED(actionData);
	ptz_debug("Gamepad button pressed: presetSelectUp");
	controls->prevPreset();
}

void PTZGamePad::presetSelectDown(short actionData)
{
	Q_UNUSED(actionData);
	ptz_debug("Gamepad button pressed: presetSelectDown");
	controls->nextPreset();
}

void PTZGamePad::presetSelectGo(short actionData)
{
	Q_UNUSED(actionData);
	ptz_debug("Gamepad button pressed: presetSelectGo");
	controls->activeSelectedPreset();
}

void PTZGamePad::presetButton(short actionData)
{
	ptz_debug("Gamepad button pressed: presetButton %i", actionData);
	controls->setPreset(actionData);
}
#endif // #ifdef OBS_PTZ_GAMEPAD
