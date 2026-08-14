/* Data transfer correctness tests - byte-for-byte equality required, no
 * tolerance. Touches only PTZUARTWrapper's public interface via TestUART -
 * see test_harness.hpp.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include "test_config.hpp"
#include "test_harness.hpp"

#include <catch_amalgamated.hpp>

#include <QRandomGenerator>

using namespace std::chrono_literals;

namespace {

QByteArray makeRandomPayload(int size, quint32 seed = 12345)
{
	QByteArray data;
	data.resize(size);
	QRandomGenerator rng(seed);
	for (int i = 0; i < size; ++i)
		data[i] = static_cast<char>(rng.bounded(256));
	return data;
}

QByteArray makeFillPayload(int size, uint8_t value)
{
	return QByteArray(size, static_cast<char>(value));
}

/* Distinguishable per-index content, used by the back-to-back test so a
 * dropped/reordered packet is easy to spot. */
QByteArray makePacket(int index)
{
	QByteArray pkt(8, '\0');
	pkt[0] = static_cast<char>(0xA5);
	pkt[1] = static_cast<char>(index & 0xFF);
	pkt[2] = static_cast<char>((index >> 8) & 0xFF);
	for (int i = 3; i < 8; ++i)
		pkt[i] = static_cast<char>((index * 7 + i) & 0xFF);
	return pkt;
}

std::vector<QByteArray> buildTestPayloads()
{
	return {
		makeFillPayload(1, 0x42),
		makeRandomPayload(14), // typical VISCA/Pelco packet size
		makeFillPayload(256, 0x00), makeFillPayload(256, 0xFF),
		makeRandomPayload(256),  // exactly the reader thread's buf[256] - boundary case
		makeRandomPayload(4096), // forces reassembly across multiple receiveBytes() deliveries
	};
}

/* Matches uart-wrapper.cpp's own standard_baud_rates table (file-static
 * there, so not directly reusable) - exactly what the plugin's UI offers. */
const int kStandardBaudRates[] = {1200, 2400, 4800, 9600, 19200, 38400, 57600, 115200};

} // namespace

TEST_CASE("UART data transfer: byte-for-byte integrity", "[data-transfer]")
{
	TestUART dut(testConfig().portA);
	TestUART peer(testConfig().portB);
	REQUIRE(openAtConfiguredBaud(dut));
	REQUIRE(openAtConfiguredBaud(peer));
	REQUIRE(pumpUntil([&] { return dut.openAttempts.back().success && peer.openAttempts.back().success; }, 2s));

	auto roundTrip = [&](const QByteArray &payload, std::chrono::milliseconds timeout) {
		SECTION("DUT -> peer")
		{
			dut.send(payload);
			REQUIRE(pumpUntil([&] { return peer.received.size() >= payload.size(); }, timeout));
			REQUIRE(peer.received == payload);
			peer.received.clear();
		}
		SECTION("peer -> DUT")
		{
			peer.send(payload);
			REQUIRE(pumpUntil([&] { return dut.received.size() >= payload.size(); }, timeout));
			REQUIRE(dut.received == payload);
			dut.received.clear();
		}
	};

	SECTION("payload sizes and patterns")
	{
		for (const auto &payload : buildTestPayloads())
			roundTrip(payload, 2s);
	}

	SECTION("across standard baud rates")
	{
		for (int rate : kStandardBaudRates) {
			dut.setBaudRate(rate);
			peer.setBaudRate(rate);
			roundTrip(makeRandomPayload(64), 2s);
		}
	}

	SECTION("rapid back-to-back sends, no gaps")
	{
		QByteArray expected;
		for (int i = 0; i < 50; ++i) {
			QByteArray pkt = makePacket(i);
			dut.send(pkt);
			expected += pkt;
		}
		REQUIRE(pumpUntil([&] { return peer.received.size() >= expected.size(); }, 5s));
		REQUIRE(peer.received == expected);
	}
}
