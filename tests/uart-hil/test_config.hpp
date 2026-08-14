/* Global test configuration - port names, baud rate, and which
 * DisconnectTrigger implementation is active - populated once by main()
 * before Catch2's Session::run(), read by every TEST_CASE.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#pragma once

#include "socat_harness.hpp"

#include <QString>

struct TestConfig {
	QString portA;
	QString portB;
	int baud = 9600;
	/* Non-owning - main() keeps the concrete SocatKillTrigger/
	 * ManualPromptTrigger alive on its own stack for the whole run. */
	DisconnectTrigger *disconnectTrigger = nullptr;
};

TestConfig &testConfig();
