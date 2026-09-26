/* PTZUsbBackend's handling of cameras that lack controls, and of stepped ranges.
 *
 * SPDX-License-Identifier: GPLv2
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

	static double ratioOf(long value, long maximum) { return ratio(value, maximum); }
	static long stepOf(long increment) { return valid_step(increment); }
};

} // namespace

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
