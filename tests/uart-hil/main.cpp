/* Hardware-in-the-loop test suite for PTZUARTWrapper - own-main() Catch2
 * entry point. See README.md for usage.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include "socat_harness.hpp"
#include "test_config.hpp"
#include "test_harness.hpp"

#include <catch_amalgamated.hpp>

#include <QCoreApplication>

#include <cstdlib>
#include <iostream>

namespace {

QString envOrEmpty(const char *name)
{
	const char *value = std::getenv(name);
	return value ? QString::fromLocal8Bit(value) : QString();
}

} // namespace

int main(int argc, char *argv[])
{
	Catch::Session session;

	std::string portA, portB;
	int baud = 9600;

	auto cli =
		session.cli() |
		Catch::Clara::Opt(portA, "port")["--port-a"]("DUT serial port (env fallback: OBS_PTZ_TEST_PORT_A; "
							     "if neither is set, defaults to a managed socat pair on "
							     "macOS/Linux)") |
		Catch::Clara::Opt(portB, "port")["--port-b"]("peer serial port (env fallback: "
							     "OBS_PTZ_TEST_PORT_B)") |
		Catch::Clara::Opt(baud, "baud")["--baud"]("baud rate for tests not sweeping baud "
							  "(env fallback: OBS_PTZ_TEST_BAUD, default 9600)");
	session.cli(cli);

	int rc = session.applyCommandLine(argc, argv);
	if (rc != 0)
		return rc;

	/* CLI wins over environment variables. */
	QString qPortA = !portA.empty() ? QString::fromStdString(portA) : envOrEmpty("OBS_PTZ_TEST_PORT_A");
	QString qPortB = !portB.empty() ? QString::fromStdString(portB) : envOrEmpty("OBS_PTZ_TEST_PORT_B");
	if (baud == 9600) {
		QString envBaud = envOrEmpty("OBS_PTZ_TEST_BAUD");
		if (!envBaud.isEmpty())
			baud = envBaud.toInt();
	}

	/* Kept alive on main()'s own stack for the whole run - session.run()
	 * below is synchronous, so this covers every TEST_CASE. */
	SocatHarness socatHarness;
	SocatKillTrigger socatTrigger(socatHarness);
	ManualPromptTrigger manualTrigger;

	if (qPortA.isEmpty() && qPortB.isEmpty()) {
#ifdef _WIN32
		std::cerr << "No --port-a/--port-b given, and there's no socat on Windows to fall back to.\n"
			     "Pass real hardware ports, e.g. --port-a=COM3 --port-b=COM4.\n";
		return 1;
#else
		if (!SocatHarness::isAvailable()) {
			std::cerr << "No --port-a/--port-b given, and `socat` was not found on PATH.\n"
				     "Install it (e.g. `brew install socat` / `apt install socat`), or pass "
				     "real hardware ports with --port-a/--port-b.\n";
			return 1;
		}
		std::cout << "No ports given - using a managed socat virtual port pair (zero hardware).\n";
		if (!socatHarness.start()) {
			std::cerr << "Failed to start socat.\n";
			return 1;
		}
		qPortA = socatHarness.portA();
		qPortB = socatHarness.portB();
		testConfig().disconnectTrigger = &socatTrigger;
#endif
	} else if (qPortA.isEmpty() || qPortB.isEmpty()) {
		std::cerr << "Both --port-a and --port-b must be given together (or neither, for socat mode).\n";
		return 1;
	} else {
		testConfig().disconnectTrigger = &manualTrigger;
	}

	testConfig().portA = qPortA;
	testConfig().portB = qPortB;
	testConfig().baud = baud;

	std::cout << "port A: " << qPortA.toStdString() << "\n";
	std::cout << "port B: " << qPortB.toStdString() << "\n";
	std::cout << "baud:   " << baud << "\n";
	std::cout << "mode:   "
		  << (testConfig().disconnectTrigger->isManual() ? "hardware (manual prompts)" : "socat (automated)")
		  << "\n\n";

	/* Needed before any PTZUARTWrapper exists - its QTimer/QThread/
	 * Qt::QueuedConnection machinery all require a running event loop
	 * (see pumpUntil() in test_harness.hpp). No GUI needed, so
	 * QCoreApplication rather than QApplication. */
	QCoreApplication app(argc, argv);

	logCapture().install();

	return session.run();
}
