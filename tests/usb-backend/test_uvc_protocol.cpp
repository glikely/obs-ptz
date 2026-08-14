/* uvc-protocol.hpp: the platform independent pieces of talking UVC to a camera.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include <catch_amalgamated.hpp>

#include <cstdint>
#include <vector>

#include "uvc-protocol.hpp"

TEST_CASE("an AVFoundation unique ID of a USB camera is split into location, vendor and product", "[uvc]")
{
	SECTION("a location ID that lost its leading zero")
	{
		auto id = uvc::parse_avfoundation_unique_id("0x1420000046d0825");
		REQUIRE(id);
		CHECK(id->location_id == 0x1420000u);
		CHECK(id->vendor_id == 0x046d);
		CHECK(id->product_id == 0x0825);
	}
	SECTION("a full eight digit location ID")
	{
		auto id = uvc::parse_avfoundation_unique_id("0x11300000046d082d");
		REQUIRE(id);
		CHECK(id->location_id == 0x11300000u);
		CHECK(id->vendor_id == 0x046d);
		CHECK(id->product_id == 0x082d);
	}
	SECTION("a camera seen in the field")
	{
		auto id = uvc::parse_avfoundation_unique_id("0x210000036050064");
		REQUIRE(id);
		CHECK(id->location_id == 0x2100000u);
		CHECK(id->vendor_id == 0x3605);
		CHECK(id->product_id == 0x0064);
	}
	SECTION("upper case is fine")
	{
		auto id = uvc::parse_avfoundation_unique_id("0X1420000046D0825");
		REQUIRE(id);
		CHECK(id->vendor_id == 0x046d);
	}
}

TEST_CASE("an ID that isn't a USB camera's is refused", "[uvc]")
{
	/* the built-in camera and virtual cameras have UUIDs */
	CHECK_FALSE(uvc::parse_avfoundation_unique_id("47B4B64B-7067-4B9C-AD2B-AE273A71F4B5"));
	CHECK_FALSE(uvc::parse_avfoundation_unique_id(""));
	CHECK_FALSE(uvc::parse_avfoundation_unique_id("1420000046d0825")); /* no 0x */
	CHECK_FALSE(uvc::parse_avfoundation_unique_id("0x1234"));          /* too short to hold the three fields */
	CHECK_FALSE(uvc::parse_avfoundation_unique_id("0xzz1420000046d0825"));
	CHECK_FALSE(uvc::parse_avfoundation_unique_id("0x11111111111111111")); /* too long */
}

namespace {

/* A configuration descriptor with a VideoControl interface (number `vc`) holding
 * a Camera Terminal of entity `terminal`, then a VideoStreaming interface with
 * a look-alike, as a real one has. The descriptors are copied byte for byte from
 * the layout in the UVC spec. */
std::vector<uint8_t> configuration(uint8_t vc, uint8_t terminal, uint8_t terminal_type_high = 0x02)
{
	return {
		9,        2,    0,
		0,        2,    1,
		0,        0x80, 50, /* configuration */
		9,        4,    vc,
		0,        1,    0x0E,
		0x01,     0,    0, /* VideoControl interface */
		13,       0x24, 1,
		0,        1,    0x20,
		0x03,     0,    0,
		0,        0,    1,
		1, /* class-specific header */
		18,       0x24, 2,
		terminal, 0x01, terminal_type_high,
		0,        0, /* input terminal, type ITT_CAMERA */
		0,        0,    0,
		0,        0,    0,
		3,        0x0a, 0x0a,
		0, /* focal lengths, bmControls */
		9,        4,    uint8_t(vc + 1),
		0,        0,    0x0E,
		0x02,     0,    0, /* VideoStreaming interface */
		18,       0x24, 2,
		9,        0x01, 0x02,
		0,        0,    0,
		0,        0,    0,
		0,        0,    3,
		0,        0,    0, /* must be ignored here */
	};
}

} // namespace

TEST_CASE("the Camera Terminal is found in a configuration descriptor", "[uvc]")
{
	auto config = configuration(0, 7);
	auto terminal = uvc::find_camera_terminal(config.data(), config.size());
	REQUIRE(terminal);
	CHECK(terminal->interface_number == 0);
	CHECK(terminal->terminal_id == 7);

	SECTION("on whichever interface the VideoControl interface is")
	{
		config = configuration(2, 4);
		terminal = uvc::find_camera_terminal(config.data(), config.size());
		REQUIRE(terminal);
		CHECK(terminal->interface_number == 2);
		CHECK(terminal->terminal_id == 4);
	}
	SECTION("but only if it is a camera")
	{
		config = configuration(0, 7, 0x04); /* 0x0401, a composite connector */
		CHECK_FALSE(uvc::find_camera_terminal(config.data(), config.size()));
	}
}

TEST_CASE("a damaged descriptor is not read past its end", "[uvc]")
{
	auto config = configuration(0, 7);

	for (size_t length : {size_t(0), size_t(1), size_t(5), size_t(30), size_t(40)})
		CHECK_FALSE(uvc::find_camera_terminal(config.data(), length));

	SECTION("a descriptor that claims to be longer than what's left")
	{
		config[9 + 9 + 13] = 200; /* the terminal's length */
		CHECK_FALSE(uvc::find_camera_terminal(config.data(), config.size()));
	}
	SECTION("a descriptor with a length of zero doesn't loop forever")
	{
		config[9] = 0;
		CHECK_FALSE(uvc::find_camera_terminal(config.data(), config.size()));
	}
}

TEST_CASE("control payloads are little endian", "[uvc]")
{
	uint8_t pan_tilt[uvc::PANTILT_LEN];
	uvc::put_le32(pan_tilt, -3600);
	uvc::put_le32(pan_tilt + 4, 7200);
	CHECK(uvc::get_le32(pan_tilt) == -3600);
	CHECK(uvc::get_le32(pan_tilt + 4) == 7200);
	CHECK(pan_tilt[0] == 0xf0); /* -3600 is 0xfffff1f0 */
	CHECK(pan_tilt[1] == 0xf1);
	CHECK(pan_tilt[3] == 0xff);

	uint8_t zoom[uvc::ZOOM_LEN];
	uvc::put_le16(zoom, 0x1234);
	CHECK(zoom[0] == 0x34);
	CHECK(zoom[1] == 0x12);
	CHECK(uvc::get_le16(zoom) == 0x1234);
}

TEST_CASE("a control request addresses its entity and selector", "[uvc]")
{
	/* wIndex: the entity in the high byte, the interface in the low; wValue: the selector in the high byte */
	CHECK(uvc::control_index(7, 0) == 0x0700);
	CHECK(uvc::control_index(1, 2) == 0x0102);
	CHECK(uvc::control_value(uvc::CT_PANTILT_ABSOLUTE) == 0x0d00);
	CHECK(uvc::control_value(uvc::CT_ZOOM_ABSOLUTE) == 0x0b00);
}
