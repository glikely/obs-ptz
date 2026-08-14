/* Reconnect logic test - disconnect/reconnect triggered either by killing
 * and restarting a managed socat process (socat mode) or by prompting a
 * human to physically unplug/replug (hardware mode) - see
 * TestConfig::disconnectTrigger in test_config.hpp. The test body itself
 * is identical either way.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include "test_config.hpp"
#include "test_harness.hpp"

#include <catch_amalgamated.hpp>

#include <QRandomGenerator>

#include <iostream>

using namespace std::chrono_literals;

namespace {

QByteArray makeRandomPayload(int size, quint32 seed)
{
	QByteArray data;
	data.resize(size);
	QRandomGenerator rng(seed);
	for (int i = 0; i < size; ++i)
		data[i] = static_cast<char>(rng.bounded(256));
	return data;
}

} // namespace

TEST_CASE("UART reconnect after disconnect/reconnect", "[reconnect]")
{
	DisconnectTrigger *trigger = testConfig().disconnectTrigger;
	REQUIRE(trigger != nullptr);
	if (trigger->isManual())
		std::cout << "\nThis test needs you to physically unplug and replug the DUT's serial adapter "
			     "when prompted.\n";

	TestUART dut(testConfig().portA);
	TestUART peer(testConfig().portB);
	REQUIRE(openAtConfiguredBaud(dut));
	REQUIRE(openAtConfiguredBaud(peer));
	REQUIRE(pumpUntil([&] { return dut.openAttempts.back().success && peer.openAttempts.back().success; }, 2s));

	/* 1. Sanity: data transfer works pre-disconnect, not just that
	 * open() returned true. */
	QByteArray pre = makeRandomPayload(32, 1);
	dut.send(pre);
	REQUIRE(pumpUntil([&] { return peer.received.size() >= pre.size(); }, 2s));
	REQUIRE(peer.received == pre);
	peer.received.clear();

	int logCountBefore = logCapture().snapshot().size();

	// 2.
	trigger->triggerDisconnect();

	/* 3. Disconnect detected: QSerialPort::errorOccurred() fires on the
	 * killed socat pty, handleError() logs "UART %s error: ..." - poll
	 * the captured log rather than TestUART::open(), since open() isn't
	 * called again until the retry timer's next tick (step 4). Scoped to
	 * the DUT's own port name: in socat mode, killing the shared socat
	 * process disconnects BOTH ends of the pair, so the peer logs its
	 * own error too - a plain "error" search could be satisfied by that
	 * instead of the DUT's. */
	QString dutError = QString("UART %1 error:").arg(dut.portName());
	REQUIRE(pumpUntil([&] { return containsMessage(logCapture().snapshot(), logCountBefore, dutError); }, 10s));

	/* 4. Retry timer visibly retrying: at least two more open()
	 * attempts recorded on TestUART, both failing, roughly
	 * reconnect_poll_interval_ms (2000ms) apart. */
	size_t attemptsAtDisconnect = dut.openAttempts.size();
	REQUIRE(pumpUntil([&] { return dut.openAttempts.size() >= attemptsAtDisconnect + 2; }, 8s));
	for (size_t i = attemptsAtDisconnect; i < dut.openAttempts.size(); ++i)
		CHECK_FALSE(dut.openAttempts[i].success);
	auto gap = dut.openAttempts[attemptsAtDisconnect + 1].timestampMs -
		   dut.openAttempts[attemptsAtDisconnect].timestampMs;
	CHECK(gap > 1500); // ~2000ms with slack, not a tight spin loop

	// 5.
	trigger->triggerReconnect();

	/* 6. Reconnect succeeds within a bounded number of retry-timer
	 * ticks. Also wait for the peer: in socat mode restarting the
	 * shared process brings back both ends of the pair, so the peer
	 * independently went through its own disconnect/reconnect cycle
	 * too and needs to finish it before data transfer can work again.
	 * In hardware mode only the DUT's adapter was ever touched, so
	 * peer's own openAttempts never changed and this half of the
	 * check is already satisfied. */
	REQUIRE(pumpUntil([&] { return dut.openAttempts.back().success && peer.openAttempts.back().success; }, 10s));

	/* 7. Data transfer still works correctly *after* reconnect, in both
	 * directions - the point explicitly called out, not just that
	 * open() returned true. */
	QByteArray post1 = makeRandomPayload(32, 2);
	dut.send(post1);
	REQUIRE(pumpUntil([&] { return peer.received.size() >= post1.size(); }, 2s));
	REQUIRE(peer.received == post1);

	QByteArray post2 = makeRandomPayload(256, 3);
	peer.send(post2);
	dut.received.clear();
	REQUIRE(pumpUntil([&] { return dut.received.size() >= post2.size(); }, 2s));
	REQUIRE(dut.received == post2);
}

TEST_CASE("UART reconnects when the port doesn't exist at the very first open", "[reconnect]")
{
	/* No hardware/socat needed - a port name that never resolves to a
	 * real device reproduces the exact bug: a device plugged in only
	 * after OBS has already started (so open() fails on its one and only
	 * call) previously never got retried at all, since reconnect_timer
	 * was only ever started from handleError() - which requires a prior
	 * successful open(). open() itself must also start it. */
#ifdef _WIN32
	QString bogusPort = "COM255";
#else
	QString bogusPort = "/dev/obs-ptz-test-definitely-does-not-exist";
#endif
	TestUART dut(bogusPort);

	REQUIRE_FALSE(dut.open());
	REQUIRE(dut.openAttempts.size() == 1);
	CHECK_FALSE(dut.openAttempts[0].success);

	/* At least two more retry-timer attempts, roughly
	 * reconnect_poll_interval_ms (2000ms) apart - same shape as the
	 * post-disconnect retry check above. */
	REQUIRE(pumpUntil([&] { return dut.openAttempts.size() >= 3; }, 8s));
	for (size_t i = 1; i < dut.openAttempts.size(); ++i)
		CHECK_FALSE(dut.openAttempts[i].success);
	auto gap = dut.openAttempts[2].timestampMs - dut.openAttempts[1].timestampMs;
	CHECK(gap > 1500);
}
