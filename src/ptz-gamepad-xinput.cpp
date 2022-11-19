/* PTZ Gamepad Windows Support
* Copyright 2022 Eric Schmidt <ericbschmidt@gmail.com>
* SPDX-License-Identifier: GPLv2
* 
* This class implements support for Xbox controllers using XInput. Enable in the settings.
* Default buttons/axis:
* left axis - zoom
* right axis - pan/tilt
* dpad up - camera select up
* dpad down - camera select down
* dpad left - preset up
* dpad right - preset down
* start - activate selected preset
* A - preset 1
* B - preset 2
* X - preset 3
* Y - preset 4
* left shoulder - preset 5
* right shoulder - preset 6
* back - preset 7
* Thumb L - preset 8
* Thumb R - preset 9
* 
* Known Issue:
* It can issue too many commands depending upon use which can overflow the visca over ip command buffer.
* This is not noticeable usually, but the command to stop the gamepad could be missed in rare cases.
* 
* Wishlist Improvements:
* Add a button mapping configuration.
* Add a modifier button to add more presets.
* Add focus controls as an option.
* Add deadzone support.
* Support multiple gamepads.
* Support different types of gamepads. (DirectInput)
* Add support for linux and mac.
*/

#include "ptz-device.hpp"
#include "ptz-gamepad-xinput.hpp"

static constexpr unsigned short GAMEPAD_POLL_MS = 5;
static constexpr int INVALID_GAMEPAD_ID = -1;

typedef unsigned short ButtonXInput;

struct ButtonXInputMap {
	PTZGamepadButton button;
	ButtonXInput xinputButton;
};

ButtonXInputMap buttonsXInput[] = {
	{GAMEPAD_DPAD_UP, XINPUT_GAMEPAD_DPAD_UP},
	{GAMEPAD_DPAD_DOWN, XINPUT_GAMEPAD_DPAD_DOWN},
	{GAMEPAD_DPAD_LEFT, XINPUT_GAMEPAD_DPAD_LEFT},
	{GAMEPAD_DPAD_RIGHT, XINPUT_GAMEPAD_DPAD_RIGHT},
	{GAMEPAD_LEFT_SHOULDER, XINPUT_GAMEPAD_LEFT_SHOULDER},
	{GAMEPAD_RIGHT_SHOULDER, XINPUT_GAMEPAD_RIGHT_SHOULDER},
	{GAMEPAD_START, XINPUT_GAMEPAD_START},
	{GAMEPAD_A, XINPUT_GAMEPAD_A},
	{GAMEPAD_B, XINPUT_GAMEPAD_B},
	{GAMEPAD_X, XINPUT_GAMEPAD_X},
	{GAMEPAD_Y, XINPUT_GAMEPAD_Y},
	{GAMEPAD_LEFT_THUMB, XINPUT_GAMEPAD_LEFT_THUMB},
	{GAMEPAD_RIGHT_THUMB, XINPUT_GAMEPAD_RIGHT_THUMB},
	{GAMEPAD_BACK, XINPUT_GAMEPAD_BACK},
};

static_assert((sizeof(buttonsXInput) / sizeof(ButtonXInputMap)) ==
		      PTZGamepadButton::GAMEPAD_BUTTON_COUNT,
	      "PTZGamepadButton needs to match buttonsXInput");

PTZGamepadThread::PTZGamepadThread(PTZGamePad *gamepadInput)
{
	gamepad = gamepadInput;
	stop = false;
}

void PTZGamepadThread::run()
{
	while (!stop.testAndSetAcquire(1, 0)) {
		gamepad->gamepadTick();
		Sleep(GAMEPAD_POLL_MS);
	}
}

PTZGamePad::PTZGamePad()
{
	status = GAMEPAD_STATUS_BLANK;
	ZeroMemory(&state, sizeof(XINPUT_STATE));
	lastPacketNumber = 0;
	frameCount = 0;
	resetButtons();

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
	leftStickX = 0.0f;
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
	if (gamepadID == INVALID_GAMEPAD_ID) {
		frameCount++;
		if (frameCount < NUM_FRAMES_CONTROLLER_CHECK)
			return;

		initializeGamepads();
		frameCount = 0;
		if (gamepadID == INVALID_GAMEPAD_ID) {
			status = GAMEPAD_STATUS_NONE;
			emit uiGamepadStatusChanged(status);
			return;
		} else {
			status = GAMEPAD_STATUS_FOUND;
			emit uiGamepadStatusChanged(status);
		}
	}

	ZeroMemory(&state, sizeof(XINPUT_STATE));

	const DWORD dwResult = XInputGetState(gamepadID, &state);

	// check for controller disconnect
	if (dwResult != ERROR_SUCCESS) {
		gamepadID = INVALID_GAMEPAD_ID;
		resetButtons();
		emit rightAxisChanged(rightStickX, rightStickY);
		emit leftAxisChanged(leftStickX, leftStickY);
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

void PTZGamePad::convertStickInput(short thumb, double deadzone,
				   double *outStick)
{
	assert(outStick);

	const float norm = fmaxf(-1, (float)thumb / 32767);

	*outStick = (abs(norm) < deadzone
			     ? 0
			     : (abs(norm) - deadzone) * (norm / abs(norm)));

	if (deadzone > 0.0f && deadzone < 1.0f)
		(*outStick) *= 1 / (1 - deadzone);
}

void PTZGamePad::inputHandleRightStick()
{
	const double lastRightStickX = rightStickX;
	const double lastRightStickY = rightStickY;

	convertStickInput(state.Gamepad.sThumbRX, DEADZONE_X, &rightStickX);
	convertStickInput(state.Gamepad.sThumbRY, DEADZONE_Y, &rightStickY);

	if (lastRightStickX != rightStickX || lastRightStickY != rightStickY) {
		ptz_debug("PTZGamepad: right stick changed x=%.3f, y=%.3f",
			  rightStickX, rightStickY);
		emit rightAxisChanged(rightStickX, rightStickY);
	}
}

void PTZGamePad::inputHandleLeftStick()
{
	const double lastLeftStickX = leftStickX;
	const double lastLeftStickY = leftStickY;

	convertStickInput(state.Gamepad.sThumbLX, DEADZONE_X, &leftStickX);
	convertStickInput(state.Gamepad.sThumbLY, DEADZONE_Y, &leftStickY);

	if (lastLeftStickX != leftStickX || lastLeftStickY != leftStickY) {
		ptz_debug("PTZGamepad: left stick changed x=%.3f, y=%.3f",
			  leftStickX, leftStickY);
		emit leftAxisChanged(leftStickX, leftStickY);
	}
}

void PTZGamePad::inputHandleButtons()
{
	for (int buttonIndex = 0; buttonIndex < GAMEPAD_BUTTON_COUNT;
	     ++buttonIndex) {
		const ButtonXInputMap *buttonMap = &buttonsXInput[buttonIndex];
		const bool buttonPressedLast =
			((buttonsDown & buttonMap->xinputButton) != 0);
		const bool buttonPressedNow = ((state.Gamepad.wButtons &
						buttonMap->xinputButton) != 0);
		if (!buttonPressedLast && buttonPressedNow) {
			ptz_debug("PTZGamepad: button pressed: %d",
				  buttonMap->button);
			emit buttonDownChanged(buttonMap->button);
		}
	}

	buttonsDown = state.Gamepad.wButtons;
}

void PTZGamePad::setGamepadEnabled(bool enabled)
{
	if (enabled) {
		initializeGamepads();
		if (gamepadID != INVALID_GAMEPAD_ID) {
			status = GAMEPAD_STATUS_FOUND;
			emit uiGamepadStatusChanged(status);
			startThread();
		} else {
			status = GAMEPAD_STATUS_NONE;
			emit uiGamepadStatusChanged(status);
			stopThread();
		}
	} else {
		gamepadID = INVALID_GAMEPAD_ID;
		status = GAMEPAD_STATUS_BLANK;
		emit uiGamepadStatusChanged(status);
		stopThread();
	}
}

void PTZGamePad::initializeGamepads()
{
	gamepadID = INVALID_GAMEPAD_ID;
	for (DWORD i = 0; i < XUSER_MAX_COUNT; i++) {
		ZeroMemory(&state, sizeof(XINPUT_STATE));

		const DWORD dwResult = XInputGetState(i, &state);

		if (dwResult == ERROR_SUCCESS) {

			gamepadID = i;
			break;
		}
	}
}