/* PTZUsbBackend's handling of cameras that lack controls, and of stepped ranges.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include <catch_amalgamated.hpp>

#include <cmath>

#include "ptz-usb-backend.hpp"

namespace {

/* Records what the base class asks the hardware to do */
class FakeBackend : public PTZUsbBackend {
public:
	int calls = 0;
	long pan_value = 0, tilt_value = 0, zoom_value = 0, focus_value = 0;
	bool autofocus = false;

	FakeBackend(PtzUsbCamLimits mn = {}, PtzUsbCamLimits mx = {}, PtzUsbCamLimits st = {1, 1, 1, 1})
	{
		min = mn;
		max = mx;
		step = st;
	}

	bool internal_pan(long v) override
	{
		calls++;
		pan_value = v;
		return true;
	}
	bool internal_tilt(long v) override
	{
		calls++;
		tilt_value = v;
		return true;
	}
	bool internal_zoom(long v) override
	{
		calls++;
		zoom_value = v;
		return true;
	}
	bool internal_focus(bool a, long v) override
	{
		calls++;
		autofocus = a;
		focus_value = v;
		return true;
	}
	bool isValid() const override { return true; }

	int speed_calls = 0;
	long pan_speed = 0, tilt_speed = 0;
	bool internal_pantilt_speed(long p, long t) override
	{
		speed_calls++;
		pan_speed = p;
		tilt_speed = t;
		return true;
	}
	void setPanTiltSpeeds(PtzUsbCamSpeeds s)
	{
		has_pantilt_speed = true;
		speeds = s;
	}
	void setAutofocus() { has_autofocus = true; }

	static double ratioOf(long value, long maximum) { return ratio(value, maximum); }
	static long stepOf(long increment) { return valid_step(increment); }
};

} // namespace

TEST_CASE("by default a backend is alive if it is valid", "[usb-backend]")
{
	struct Gone : FakeBackend {
		bool isValid() const override { return false; }
	};

	FakeBackend here;
	Gone gone;
	CHECK(here.checkAlive());
	CHECK_FALSE(gone.checkAlive());
}

TEST_CASE("a backend can't read the camera's position unless it says so, and reports its ranges", "[usb-backend]")
{
	FakeBackend cam({-100, -50, 0, 0}, {100, 60, 200, 300}, {5, 1, 1, 1});

	CHECK_FALSE(cam.refreshPosition());
	CHECK(cam.getMin().pan == -100);
	CHECK(cam.getMax().tilt == 60);
	CHECK(cam.getMax().zoom == 200);
	CHECK(cam.getStep().pan == 5);
}

TEST_CASE("ratio() does not divide by a missing range", "[usb-backend]")
{
	CHECK(FakeBackend::ratioOf(50, 100) == 0.5);
	CHECK(FakeBackend::ratioOf(5, 0) == 0.0);
	CHECK(std::isfinite(FakeBackend::ratioOf(-5, 0)));
}

TEST_CASE("valid_step() never returns less than 1", "[usb-backend]")
{
	CHECK(FakeBackend::stepOf(0) == 1);
	CHECK(FakeBackend::stepOf(-3) == 1);
	CHECK(FakeBackend::stepOf(1) == 1);
	CHECK(FakeBackend::stepOf(3600) == 3600);
}

TEST_CASE("a camera with no controls is not moved", "[usb-backend]")
{
	FakeBackend cam;

	CHECK_FALSE(cam.pan(0.5));
	CHECK_FALSE(cam.tilt(0.5));
	CHECK_FALSE(cam.zoom(0.5));
	CHECK_FALSE(cam.focus(0.5));

	CHECK(cam.calls == 0);
	CHECK(cam.getPan() == 0.0);
	CHECK(cam.getTilt() == 0.0);
	CHECK(cam.getZoom() == 0.0);
	CHECK(cam.getFocus() == 0.0);
}

TEST_CASE("autofocus does not depend on a focus range", "[usb-backend]")
{
	FakeBackend cam;

	CHECK(cam.setAutoFocus(true));
	CHECK(cam.calls == 1);
	CHECK(cam.autofocus);
}

TEST_CASE("only the axes a camera has are moved", "[usb-backend]")
{
	FakeBackend cam({-100, 0, 0, 0}, {100, 0, 0, 0});

	CHECK(cam.pan(0.5));
	CHECK(cam.pan_value == 50);
	CHECK_FALSE(cam.tilt(0.5));
	CHECK_FALSE(cam.zoom(0.5));
	CHECK_FALSE(cam.focus(0.5));
	CHECK(cam.calls == 1);
}

TEST_CASE("an axis with min == max has no range", "[usb-backend]")
{
	FakeBackend cam({0, 0, 200, 0}, {0, 0, 200, 0});

	CHECK_FALSE(cam.zoom(1.0));
	CHECK(cam.calls == 0);
}

TEST_CASE("values are rounded onto the camera's step", "[usb-backend]")
{
	/* UVC: pan and tilt in arc seconds with a 3600 step, zoom that starts above 0 */
	FakeBackend cam({-180000, -90000, 100, 0}, {180000, 90000, 500, 255}, {3600, 3600, 1, 5});

	SECTION("a value already on a step is unchanged")
	{
		CHECK(cam.pan(0.5));
		CHECK(cam.pan_value == 90000);
	}
	SECTION("other values are rounded to the nearest step above the minimum")
	{
		CHECK(cam.pan(0.333));
		CHECK((cam.pan_value + 180000) % 3600 == 0);
		CHECK(cam.pan_value == 61200);
	}
	SECTION("values are clamped to the range")
	{
		CHECK(cam.pan(2.0));
		CHECK(cam.pan_value == 180000);
		CHECK(cam.pan(-2.0));
		CHECK(cam.pan_value == -180000);
		CHECK(cam.zoom(0.0));
		CHECK(cam.zoom_value == 100);
		CHECK(cam.zoom(1.0));
		CHECK(cam.zoom_value == 500);
	}
	SECTION("focus steps too")
	{
		CHECK(cam.focus(0.5));
		CHECK(cam.focus_value % 5 == 0);
	}
}

TEST_CASE("a camera with no pan/tilt position is driven by speed", "[usb-backend]")
{
	FakeBackend b({0, 0, 0, 0}, {0, 0, 100, 0});
	b.setPanTiltSpeeds({1, 7, 1, 3});
	REQUIRE(b.usesPanTiltSpeed());

	CHECK(b.pantiltSpeed(0.5, -1.0));
	CHECK(b.pan_speed == 4); /* halfway from 1 to 7 */
	CHECK(b.tilt_speed == -3);

	CHECK(b.pantiltSpeed(0.01, 0));
	CHECK(b.pan_speed == 1); /* moving at all is at least the slowest */
	CHECK(b.tilt_speed == 0);

	CHECK(b.pantiltSpeed(-2.0, 0));
	CHECK(b.pan_speed == -7);

	CHECK(b.pantiltSpeed(0, 0));
	CHECK(b.pan_speed == 0);
	CHECK(b.tilt_speed == 0);

	/* and it has no position to be sent to */
	int calls = b.calls;
	CHECK_FALSE(b.pan(0.5));
	CHECK_FALSE(b.tilt(0.5));
	CHECK(b.calls == calls);
}

TEST_CASE("only a change of speed the camera can tell apart is sent", "[usb-backend]")
{
	FakeBackend b;
	b.setPanTiltSpeeds({1, 1, 1, 1}); /* one speed: just a direction */

	CHECK(b.pantiltSpeed(0.2, 0));
	CHECK(b.pantiltSpeed(0.9, 0));
	CHECK(b.speed_calls == 1);
	CHECK(b.pan_speed == 1);

	CHECK(b.pantiltSpeed(-0.3, 0));
	CHECK(b.speed_calls == 2);
	CHECK(b.pan_speed == -1);
}

TEST_CASE("a camera that doesn't give its speeds still moves", "[usb-backend]")
{
	FakeBackend b;
	b.setPanTiltSpeeds({0, 0, 0, 0});

	CHECK(b.pantiltSpeed(1.0, -0.5));
	CHECK(b.pan_speed == 1);
	CHECK(b.tilt_speed == -1);
}

TEST_CASE("a camera that can go to a position is not driven by speed", "[usb-backend]")
{
	FakeBackend b({-100, -100, 0, 0}, {100, 100, 0, 0});
	b.setPanTiltSpeeds({1, 7, 1, 7});

	CHECK_FALSE(b.usesPanTiltSpeed());
	CHECK_FALSE(b.pantiltSpeed(1.0, 0));
	CHECK(b.speed_calls == 0);
	CHECK(b.pan(0.5));
}

TEST_CASE("a camera without relative pan/tilt is not driven by speed", "[usb-backend]")
{
	FakeBackend b;
	CHECK_FALSE(b.usesPanTiltSpeed());
	CHECK_FALSE(b.pantiltSpeed(1.0, 0));
	CHECK(b.speed_calls == 0);
}

TEST_CASE("the movement modes say what the camera can do, and how pan/tilt is driven", "[usb-backend]")
{
	FakeBackend b({-170, -30, 0, 0}, {170, 90, 16384, 0}, {3600, 3600, 1, 1});
	CHECK(b.movementModes() == "pan/tilt to a position (pan -170..170 step 3600, tilt -30..90 step 3600), "
				   "zoom to a position (0..16384); pan/tilt is driven to positions");

	b.setPanTiltSpeeds({1, 7, 1, 7});
	b.setAutofocus();
	CHECK(b.movementModes() == "pan/tilt to a position (pan -170..170 step 3600, tilt -30..90 step 3600), "
				   "pan/tilt at a speed (pan 1..7, tilt 1..7), zoom to a position (0..16384), "
				   "autofocus; pan/tilt is driven to positions");
}

TEST_CASE("the movement modes of a camera with no pan/tilt position", "[usb-backend]")
{
	FakeBackend b({0, 0, 100, 0}, {0, 0, 1000, 255});
	b.setPanTiltSpeeds({1, 1, 1, 1});
	CHECK(b.movementModes() == "pan/tilt at a speed (pan 1..1, tilt 1..1), zoom to a position (100..1000), "
				   "focus to a position (0..255); pan/tilt is driven at a speed");
}

TEST_CASE("the movement modes of a camera that can't move", "[usb-backend]")
{
	FakeBackend b;
	CHECK(b.movementModes() == "no pan, tilt, zoom or focus");
}
