/* Test harness for the PTZUARTWrapper hardware-in-the-loop test suite.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * Touches only PTZUARTWrapper's public interface - no backend-specific
 * includes (no <serial_cpp/serial.h>, no <QSerialPort>, no
 * libserialport.h) - so this file compiles unchanged against whichever
 * backend src/uart-wrapper.cpp is written against on a given branch.
 */
#pragma once

#include "uart-wrapper.hpp"

#include <QByteArray>
#include <QCoreApplication>
#include <QDateTime>
#include <QString>
#include <QVector>

#include <chrono>
#include <mutex>
#include <vector>

/* Concrete PTZUARTWrapper used for both ends of the wired link (DUT and
 * peer) - both sides run the exact same production reader-thread/
 * retry-timer/QueuedConnection machinery a real ViscaUART/PelcoUART would.
 */
class TestUART : public PTZUARTWrapper {
	Q_OBJECT

public:
	explicit TestUART(QString &port_name) : PTZUARTWrapper(port_name) {}

	struct OpenAttempt {
		bool success;
		qint64 timestampMs;
	};
	/* Only ever appended to from this object's own thread (open() is
	 * only ever called from the owner thread, whether directly by test
	 * code or via reconnect_timer's QTimer::timeout on the same
	 * thread), so no mutex needed - same reasoning as `received` below. */
	std::vector<OpenAttempt> openAttempts;

	bool open() override
	{
		bool ok = PTZUARTWrapper::open();
		openAttempts.push_back({ok, QDateTime::currentMSecsSinceEpoch()});
		return ok;
	}

	/* receiveBytes() is always invoked via Qt::QueuedConnection, i.e.
	 * always dispatched on this object's own thread - the same thread
	 * test code and pumpUntil() run on - so plain QByteArray is safe
	 * here with no mutex. */
	void receiveBytes(const QByteArray &bytes) override { received.append(bytes); }
	QByteArray received;
};

/* Opens `uart` and, if that succeeds, applies testConfig().baud - the
 * standard way every test should bring a TestUART up. A bare open() alone
 * leaves the port at serial_cpp::Serial's hardcoded 9600 default
 * regardless of what --baud/OBS_PTZ_TEST_BAUD was given, since
 * PTZUARTWrapper's constructor never applies an initial baud rate itself
 * (see uart-wrapper.cpp) - only an explicit setBaudRate() call does. */
bool openAtConfiguredBaud(TestUART &uart);

/* Pumps the Qt event loop (QCoreApplication::processEvents()) until
 * `predicate` returns true or `timeout` elapses. This is the only
 * mechanism by which TestUART::receiveBytes() (QueuedConnection delivery),
 * reconnect_timer retries, and the reader thread's queued reconnect
 * request ever actually run - must be called from the same thread that
 * constructed QCoreApplication (true for every Catch2 TEST_CASE here,
 * since Catch2 runs test cases sequentially on the thread that called
 * Catch::Session::run()). */
template<typename Predicate> bool pumpUntil(Predicate predicate, std::chrono::milliseconds timeout)
{
	auto deadline = std::chrono::steady_clock::now() + timeout;
	while (!predicate()) {
		if (std::chrono::steady_clock::now() >= deadline)
			return false;
		QCoreApplication::processEvents(QEventLoop::AllEvents, 50);
	}
	return true;
}

struct LogEntry {
	int level;
	QString message;
	qint64 timestampMs;
};

/* Installs a blog() handler that both forwards to the previously-installed
 * handler (so log output still prints live to the console - useful when a
 * human is watching a manual-prompt reconnect run) and records entries for
 * tests to poll. Needed because the reader thread's "UART %s disappeared"
 * log (uart-wrapper.cpp) is logged directly from the reader thread, not
 * via Qt::QueuedConnection like receiveBytes() is - so log capture must be
 * thread-safe on its own, unlike TestUART's members above. */
class LogCapture {
public:
	void install();
	QVector<LogEntry> snapshot() const;

private:
	static void trampoline(int lvl, const char *msg, va_list args, void *param);

	mutable std::mutex mutex_;
	QVector<LogEntry> entries_;
	void (*previousHandler_)(int, const char *, va_list, void *) = nullptr;
	void *previousParam_ = nullptr;
};

/* Global instance, installed once in main(). */
LogCapture &logCapture();

/* True if `entries` (as returned by LogCapture::snapshot()) contains a
 * message containing `substring` at index >= `fromIndex` - used to poll
 * for a specific log line appearing after a known point (e.g. the number
 * of entries captured so far when a test phase started). */
bool containsMessage(const QVector<LogEntry> &entries, int fromIndex, const QString &substring);
