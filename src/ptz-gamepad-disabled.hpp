/* PTZ Gamepad Windows Support
* Copyright 2022 Eric Schmidt <ericbschmidt@gmail.com>
* SPDX-License-Identifier: GPLv2
*/

#pragma once

#include "ptz-gamepad.hpp"

class PTZGamePad : public PTZGamePadBase {
public:
	bool isGamepadSupportEnabled() { return false; };
	void setGamepadEnabled(bool enabled) { Q_UNUSED(enabled) };
	PTZGamepadStatus getGamepadStatus()
	{
		return GAMEPAD_STATUS_NOT_SUPPORTED;
	};
};