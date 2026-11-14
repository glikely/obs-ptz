/* Unit tests for Panasonic HTTP CGI protocol helpers (panasonic-protocol.hpp).
 *
 * Copyright 2026 Grant Likely <grant.likely@secretlab.ca>
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include <catch_amalgamated.hpp>
#include "panasonic-protocol.hpp"

using Catch::Approx;
using namespace panasonic_protocol;

TEST_CASE("panasonic: speedToPanasonic maps [-1, 1] to [1, 99] with 50 as stop", "[panasonic]")
{
	REQUIRE(speedToPanasonic(0.0) == 50);
	REQUIRE(speedToPanasonic(-1.0) == 1);
	REQUIRE(speedToPanasonic(1.0) == 99);
	REQUIRE(speedToPanasonic(-0.5) == 25);
	REQUIRE(speedToPanasonic(0.5) == 75);
	REQUIRE(speedToPanasonic(-2.0) == 1);
	REQUIRE(speedToPanasonic(2.0) == 99);
}

TEST_CASE("panasonic: pan/tilt hex conversions round-trip", "[panasonic]")
{
	REQUIRE(normToPanTiltHex(0.0) == 0x8000);
	REQUIRE(normToPanTiltHex(-1.0) == 0x0001);
	REQUIRE(normToPanTiltHex(1.0) == 0xFFFF);
	REQUIRE(normToPanTiltHex(-1.5) == 0x0001);
	REQUIRE(normToPanTiltHex(1.5) == 0xFFFF);

	REQUIRE(panTiltHexToNorm(0x8000) == Approx(0.0));
	REQUIRE(panTiltHexToNorm(0x0000) == Approx(-1.0));
	REQUIRE(panTiltHexToNorm(0xFFFF) == Approx(1.0));
}

TEST_CASE("panasonic: zoom/focus hex conversions map [0, 1] to [0x555, 0xFFF]", "[panasonic]")
{
	REQUIRE(normToZoomFocusHex(0.0) == 0x555);
	REQUIRE(normToZoomFocusHex(1.0) == 0xFFF);
	REQUIRE(normToZoomFocusHex(-0.5) == 0x555);
	REQUIRE(normToZoomFocusHex(1.5) == 0xFFF);

	REQUIRE(zoomFocusHexToNorm(0x555) == Approx(0.0));
	REQUIRE(zoomFocusHexToNorm(0xFFF) == Approx(1.0));
	REQUIRE(zoomFocusHexToNorm(0x000) == Approx(0.0));
	REQUIRE(zoomFocusHexToNorm(0x1200) == Approx(1.0));

	for (double v : {0.0, 0.25, 0.5, 0.75, 1.0}) {
		uint16_t raw = normToZoomFocusHex(v);
		REQUIRE(zoomFocusHexToNorm(raw) == Approx(v).margin(0.001));
	}
}

TEST_CASE("panasonic: parseCamdataBody parses full status payload", "[panasonic]")
{
	const std::string body =
		"p1\r\n"
		"aPC8000FFFF\r\n"
		"gz555\r\n"
		"gfFFF\r\n"
		"d11\r\n";

	CamdataUpdate u = parseCamdataBody(body);
	REQUIRE(u.powerOn.has_value());
	REQUIRE(*u.powerOn == true);
	REQUIRE(u.pan.has_value());
	REQUIRE(*u.pan == Approx(0.0));
	REQUIRE(u.tilt.has_value());
	REQUIRE(*u.tilt == Approx(1.0));
	REQUIRE(u.zoom.has_value());
	REQUIRE(*u.zoom == Approx(0.0));
	REQUIRE(u.focus.has_value());
	REQUIRE(*u.focus == Approx(1.0));
	REQUIRE(u.focusAfEnabled.has_value());
	REQUIRE(*u.focusAfEnabled == true);
}

TEST_CASE("panasonic: parseCamdataBody handles standby, manual focus, case, and bad hex", "[panasonic]")
{
	const std::string body =
		"P0\r\n"
		"D10\r\n"
		"apc0001ZZZZ\r\n"
		"gzZZZ\r\n"
		"gfAAA\r\n"
		"short\r\n";

	CamdataUpdate u = parseCamdataBody(body);
	REQUIRE(u.powerOn.has_value());
	REQUIRE(*u.powerOn == false);
	REQUIRE(u.focusAfEnabled.has_value());
	REQUIRE(*u.focusAfEnabled == false);
	REQUIRE(u.pan.has_value());
	REQUIRE(*u.pan == Approx(-1.0).margin(0.0001));
	REQUIRE_FALSE(u.tilt.has_value());
	REQUIRE_FALSE(u.zoom.has_value());
	REQUIRE(u.focus.has_value());
	REQUIRE(*u.focus == Approx(zoomFocusHexToNorm(0xAAA)));
}

