/* Concurrent read/write tests - the category that directly targets the
 * reported Windows blocking-write symptom. Measures send() latency (the
 * duration of PTZUARTWrapper::send(), i.e. the underlying backend write()
 * call) under conditions designed to surface a blocking write, with real
 * numbers rather than a bare pass/fail: on Windows hardware, the "far end
 * absent" section failing its latency bound *is* the objective repro.
 *
 * Also measures read latency (time from a peer's send() to the bytes
 * showing up in receiveBytes()) - a regression test for a real bug found
 * via this suite: see "UART read latency for small payloads" below.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include "test_config.hpp"
#include "test_harness.hpp"

#include <catch_amalgamated.hpp>

#include <QRandomGenerator>

#include <algorithm>
#include <iostream>
#include <numeric>

using namespace std::chrono_literals;
using Clock = std::chrono::steady_clock;

namespace {

QByteArray makeRandomPayload(int size, quint32 seed = 6789)
{
	QByteArray data;
	data.resize(size);
	QRandomGenerator rng(seed);
	for (int i = 0; i < size; ++i)
		data[i] = static_cast<char>(rng.bounded(256));
	return data;
}

/* Runs dut.send(payload) `iterations` times, pumping the event loop briefly
 * between sends so any queued reader-thread/receiveBytes() activity keeps
 * flowing normally (send() itself is synchronous and doesn't need pumping
 * to complete, but leaving the loop fully unpumped for the whole run would
 * be an artificial condition no real caller creates). Returns the
 * per-call latency of send() itself. */
std::vector<std::chrono::milliseconds> measureSendLatencies(TestUART &dut, const QByteArray &payload, int iterations)
{
	std::vector<std::chrono::milliseconds> latencies;
	latencies.reserve(iterations);
	for (int i = 0; i < iterations; ++i) {
		auto t0 = Clock::now();
		dut.send(payload);
		auto t1 = Clock::now();
		latencies.push_back(std::chrono::duration_cast<std::chrono::milliseconds>(t1 - t0));
		QCoreApplication::processEvents(QEventLoop::AllEvents, 5);
	}
	return latencies;
}

/* Sends `payloadSize` random bytes from `sender` to `receiver` `iterations`
 * times, measuring the time from the send() call to `receiver.received`
 * reflecting the new bytes (via pumpUntil - the only way QueuedConnection-
 * delivered receiveBytes() calls actually run). This is a read-latency
 * measurement, the counterpart to measureSendLatencies() above - it
 * directly exercises PTZUARTWrapper's read timeout configuration rather
 * than its write timeout. */
std::vector<std::chrono::milliseconds> measureReceiveLatencies(TestUART &sender, TestUART &receiver, int payloadSize,
							       int iterations)
{
	std::vector<std::chrono::milliseconds> latencies;
	latencies.reserve(iterations);
	for (int i = 0; i < iterations; ++i) {
		receiver.received.clear();
		QByteArray payload = makeRandomPayload(payloadSize, static_cast<quint32>(1000 + i));
		auto t0 = Clock::now();
		sender.send(payload);
		bool ok = pumpUntil([&] { return receiver.received.size() >= payload.size(); }, 2s);
		auto t1 = Clock::now();
		REQUIRE(ok);
		latencies.push_back(std::chrono::duration_cast<std::chrono::milliseconds>(t1 - t0));
	}
	return latencies;
}

std::chrono::milliseconds percentile(std::vector<std::chrono::milliseconds> values, double p)
{
	std::sort(values.begin(), values.end());
	size_t idx = static_cast<size_t>(p * (double)(values.size() - 1));
	return values[idx];
}

void reportLatencyStats(const char *label, const std::vector<std::chrono::milliseconds> &latencies)
{
	auto sorted = latencies;
	std::sort(sorted.begin(), sorted.end());
	auto sum = std::accumulate(sorted.begin(), sorted.end(), std::chrono::milliseconds(0));
	std::cout << "[" << label << "] n=" << sorted.size() << " min=" << sorted.front().count()
		  << "ms median=" << sorted[sorted.size() / 2].count() << "ms p95=" << percentile(sorted, 0.95).count()
		  << "ms max=" << sorted.back().count() << "ms mean=" << (sum.count() / (int)sorted.size()) << "ms\n";
}

} // namespace

TEST_CASE("UART write latency under concurrent reader-thread activity", "[concurrent]")
{
	TestUART dut(testConfig().portA);
	REQUIRE(openAtConfiguredBaud(dut));
	REQUIRE(pumpUntil([&] { return dut.openAttempts.back().success; }, 2s));
	/* The reader thread is already running once open() succeeds - no
	 * separate setup needed for "a concurrent read() is in flight"; it
	 * loops uart.read() with its configured timeout for as long as the
	 * port stays open. */

	SECTION("baseline - peer alive and idle-pumped")
	{
		TestUART peer(testConfig().portB);
		REQUIRE(openAtConfiguredBaud(peer));
		REQUIRE(pumpUntil([&] { return peer.openAttempts.back().success; }, 2s));

		auto latencies = measureSendLatencies(dut, makeRandomPayload(256), 200);
		reportLatencyStats("baseline", latencies);
		CHECK(percentile(latencies, 0.95) < 100ms);
	}

	SECTION("far end absent - closest real-world 'nothing draining'")
	{
		/* Peer intentionally never opened for this section. dut.send()
		 * still goes out over a real, wired, powered UART line with
		 * nothing clocking bytes off the other end - the honest
		 * equivalent of "camera is off" or "cable to nothing",
		 * without faking anything backend-specific (a real
		 * PTZUARTWrapper's reader thread can't be paused once open,
		 * so "peer open but not reading" isn't expressible - not
		 * opening it at all is the realistic stand-in). */
		auto latencies = measureSendLatencies(dut, makeRandomPayload(256), 200);
		reportLatencyStats("far-end-absent", latencies);

		/* PTZUARTWrapper configures Timeout::simpleTimeout(100), i.e.
		 * write_timeout_constant=100 (uart-wrapper.cpp). Every send()
		 * is required to return within that bound plus generous
		 * cross-platform scheduling slack. On real Windows hardware,
		 * this CHECK failing (rather than passing near 100ms) is the
		 * objective confirmation the write genuinely blocks past its
		 * configured timeout, with real numbers instead of "appears
		 * to". */
		for (auto latency : latencies)
			CHECK(latency < 500ms);
	}

	SECTION("sustained high-rate streaming, peer alive")
	{
		TestUART peer(testConfig().portB);
		REQUIRE(openAtConfiguredBaud(peer));
		REQUIRE(pumpUntil([&] { return peer.openAttempts.back().success; }, 2s));

		/* Fires sends back-to-back as fast as possible (no inter-call
		 * pump/delay) - tests whether throughput-driven backpressure
		 * (not an absent peer) causes blocking, closer to a real
		 * rapid PTZ command burst than the idle-paced baseline. */
		std::vector<std::chrono::milliseconds> latencies;
		QByteArray payload = makeRandomPayload(256);
		for (int i = 0; i < 500; ++i) {
			auto t0 = Clock::now();
			dut.send(payload);
			auto t1 = Clock::now();
			latencies.push_back(std::chrono::duration_cast<std::chrono::milliseconds>(t1 - t0));
		}
		reportLatencyStats("sustained-streaming", latencies);
		for (auto latency : latencies)
			CHECK(latency < 500ms);

		/* A fixed 10s budget here isn't actually generous - it's simply
		 * impossible at low baud rates: 500 * 256 bytes = 128000 bytes,
		 * which alone takes ~133s to physically transmit at 9600 baud
		 * (~960 bytes/sec at 8N1). This isn't a backend/timing bug, it's
		 * the wire itself - so scale the budget with the configured baud
		 * rate (2x the theoretical transfer time, plus a flat 2s for
		 * everything else) instead of a baud-blind constant. */
		auto totalBytes = 500 * payload.size();
		auto transferTime = std::chrono::milliseconds((totalBytes * 10 * 1000) / testConfig().baud);
		REQUIRE(pumpUntil([&] { return peer.received.size() >= (qsizetype)totalBytes; },
				  transferTime * 2 + 2s));
	}
}

TEST_CASE("UART send() stays bounded while reader thread is actively receiving", "[concurrent]")
{
	/* Stronger form of "concurrent": the reader thread isn't just
	 * idling in a timed-out read(), it's actively mid-receive of a
	 * real payload from the peer at the same moment dut.send() runs. */
	TestUART dut(testConfig().portA);
	TestUART peer(testConfig().portB);
	REQUIRE(openAtConfiguredBaud(dut));
	REQUIRE(openAtConfiguredBaud(peer));
	REQUIRE(pumpUntil([&] { return dut.openAttempts.back().success && peer.openAttempts.back().success; }, 2s));

	QByteArray peerPayload = makeRandomPayload(4096, 42);
	peer.send(peerPayload);

	auto t0 = Clock::now();
	dut.send(makeRandomPayload(64, 99));
	auto latency = std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - t0);
	std::cout << "[reader-busy] send() latency=" << latency.count() << "ms\n";
	CHECK(latency < 500ms);

	/* Same baud-blind-timeout issue as the sustained-streaming section
	 * below: 4096 bytes at 9600 baud alone takes ~4.27s to physically
	 * transmit, leaving a flat 5s budget only ~0.73s of real margin.
	 * Scale it the same way. */
	auto transferTime = std::chrono::milliseconds((peerPayload.size() * 10 * 1000) / testConfig().baud);
	REQUIRE(pumpUntil([&] { return dut.received.size() >= peerPayload.size(); }, transferTime * 2 + 2s));
	REQUIRE(dut.received == peerPayload); // no corruption from the concurrent write
}

TEST_CASE("UART read latency for small payloads", "[concurrent]")
{
	/* Regression test for a real bug found via this suite: PTZUARTWrapper
	 * used to configure Timeout::simpleTimeout(100), which sets
	 * inter_byte_timeout to Timeout::max() - on the POSIX backend
	 * (shared/serial/src/impl/unix.cc), that switches Serial::read() into
	 * a "wait for the whole requested buffer to fill" mode: once any data
	 * arrived, it would sleep for (bytes still needed to fill the reader
	 * thread's 256-byte buffer) * (byte time at the configured baud rate)
	 * before actually reading it - uncapped by the configured timeout, and
	 * completely disconnected from whether more data was actually coming.
	 * A single stray byte could be held for ~265ms at 9600 baud waiting
	 * for a 256-byte buffer that may never arrive, instead of being
	 * returned immediately. PTZUARTWrapper's constructor now configures a
	 * real, small inter_byte_timeout instead, which skips that code path
	 * entirely; this asserts it stays that way. */
	TestUART dut(testConfig().portA);
	TestUART peer(testConfig().portB);
	REQUIRE(openAtConfiguredBaud(dut));
	REQUIRE(openAtConfiguredBaud(peer));
	REQUIRE(pumpUntil([&] { return dut.openAttempts.back().success && peer.openAttempts.back().success; }, 2s));

	SECTION("single-byte payloads arrive promptly, not held for a full 256-byte buffer")
	{
		auto latencies = measureReceiveLatencies(peer, dut, 1, 50);
		reportLatencyStats("read-latency-1-byte", latencies);
		for (auto latency : latencies)
			CHECK(latency < 200ms);
	}

	SECTION("small multi-byte payloads (typical VISCA/Pelco packet size) arrive promptly")
	{
		auto latencies = measureReceiveLatencies(peer, dut, 14, 50);
		reportLatencyStats("read-latency-14-byte", latencies);
		for (auto latency : latencies)
			CHECK(latency < 200ms);
	}
}
