/* Zero-hardware test mode: a socat-created virtual serial port pair, with
 * disconnect/reconnect simulated by killing and restarting socat at the
 * same stable symlink path - the exact technique already proven in
 * scripts/test-uart-disconnect.sh, ported to C++/QProcess so it can be
 * driven from inside the test binary instead of a wrapping shell script.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * This file (and only this file, plus its .cpp) is allowed to know "socat"
 * exists - it's a macOS/Linux-only, not-backend-specific concern, kept
 * separate from test_harness.hpp's backend-agnostic PTZUARTWrapper-only
 * surface.
 */
#pragma once

#include <QProcess>
#include <QString>
#include <QTemporaryDir>

/* Manages one socat subprocess creating a linked pty pair. Not available on
 * Windows - callers must check isAvailable() first. */
class SocatHarness {
public:
	SocatHarness();
	~SocatHarness();

	static bool isAvailable();

	/* Launches socat and waits (bounded, milliseconds) for both stable
	 * symlink paths to appear. Returns false on timeout/failure. */
	bool start(int waitMs = 5000);

	/* Kills the current socat process (SIGKILL-equivalent via
	 * QProcess::kill(), matching scripts/test-uart-disconnect.sh's
	 * SIGKILL-only discipline) - simulates the DUT's adapter
	 * disappearing. Leaves the symlink paths themselves untouched as
	 * strings (portA()/portB() keep returning the same values); only
	 * what they resolve to goes away. */
	void kill();

	/* Starts a fresh socat process at the exact same symlink paths -
	 * simulates the same physical device reappearing. */
	bool restart(int waitMs = 5000);

	QString portA() const { return vport1_; }
	QString portB() const { return vport2_; }

private:
	bool waitForSymlinks(int waitMs);

	QTemporaryDir tempDir_;
	QString vport1_;
	QString vport2_;
	QProcess process_;
};

/* Common interface so test_reconnect.cpp doesn't need to know whether it's
 * driving a managed socat process or a human at the keyboard. */
class DisconnectTrigger {
public:
	virtual ~DisconnectTrigger() = default;
	virtual void triggerDisconnect() = 0;
	virtual void triggerReconnect() = 0;
	/* Which mode is active is only known at runtime (it depends on
	 * whether --port-a/--port-b were given), so it can't drive a
	 * compile-time Catch2 tag - test_reconnect.cpp uses this instead to
	 * print a heads-up before blocking on stdin. To skip the reconnect
	 * test entirely regardless of mode, just run
	 * `uart-hil-tests [data-transfer],[concurrent]` (positive tag
	 * selection) rather than trying to exclude it by mode. */
	virtual bool isManual() const = 0;
};

class SocatKillTrigger : public DisconnectTrigger {
public:
	explicit SocatKillTrigger(SocatHarness &harness) : harness_(harness) {}
	void triggerDisconnect() override { harness_.kill(); }
	void triggerReconnect() override { harness_.restart(); }
	bool isManual() const override { return false; }

private:
	SocatHarness &harness_;
};

class ManualPromptTrigger : public DisconnectTrigger {
public:
	void triggerDisconnect() override;
	void triggerReconnect() override;
	bool isManual() const override { return true; }
};
