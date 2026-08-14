/* SoftViewport: speeds, and eased moves.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include <catch_amalgamated.hpp>

#include <cmath>
#include <limits>

#include "ptz-soft-viewport.hpp"

using Catch::Approx;

namespace {

/* maxZoom 4, 0.5 pan units a second at full speed, a second per eased move.
 * Starts fully zoomed in: the frame has no pan or tilt range at zoom 0, so
 * nothing moves there. */
SoftViewport zoomedIn(double zoom = 1.0)
{
	SoftViewport vp;
	SoftViewportConfig config;
	config.maxZoom = 4.0;
	config.panTiltRate = 0.5;
	config.zoomRate = 0.5;
	config.recallSeconds = 1.0;
	vp.setConfig(config);
	vp.jumpTo({0, 0, zoom});
	return vp;
}

} // namespace

TEST_CASE("SoftViewport speeds", "[motion]")
{
	SECTION("integrate the speed and the rate")
	{
		SoftViewport vp = zoomedIn(1.0);
		vp.setSpeeds(1, 0, 0);
		vp.tick(0.1);
		/* 0.5 a second, over the scale of 4 */
		CHECK(vp.position().pan == Approx(0.5 / 4 * 0.1));
		CHECK(vp.moving());
	}

	SECTION("pan and tilt slow as the zoom goes in")
	{
		SoftViewport wide = zoomedIn(0.5);
		SoftViewport tight = zoomedIn(1.0);
		wide.setSpeeds(1, 0, 0);
		tight.setSpeeds(1, 0, 0);
		wide.tick(0.1);
		tight.tick(0.1);
		/* scales 2 and 4 */
		CHECK(wide.position().pan == Approx(2 * tight.position().pan));
	}

	SECTION("zoom moves by its own rate, over the frame's whole range")
	{
		SoftViewport vp = zoomedIn(0.0);
		vp.setSpeeds(0, 0, 1);
		vp.tick(0.1);
		CHECK(vp.position().zoom == Approx(0.5 * 0.1));
		for (int i = 0; i < 100; i++)
			vp.tick(0.1);
		CHECK(vp.position().zoom == Approx(1.0));
	}

	SECTION("stop on the edge, and do not wind up there")
	{
		SoftViewport vp = zoomedIn(1.0);
		vp.setSpeeds(1, 0, 0);
		for (int i = 0; i < 100; i++)
			vp.tick(0.1);
		CHECK(vp.position().pan == Approx(0.75));
		vp.setSpeeds(-1, 0, 0);
		vp.tick(0.1);
		CHECK(vp.position().pan == Approx(0.75 - 0.5 / 4 * 0.1));
	}

	SECTION("a stalled frame is not a jump")
	{
		SoftViewport a = zoomedIn(1.0), b = zoomedIn(1.0);
		a.setSpeeds(1, 0, 0);
		b.setSpeeds(1, 0, 0);
		a.tick(5000);
		b.tick(0.1);
		CHECK(a.position().pan == Approx(b.position().pan));
	}

	SECTION("stop ends them")
	{
		SoftViewport vp = zoomedIn(1.0);
		vp.setSpeeds(1, 1, 0);
		vp.tick(0.1);
		vp.stop();
		SoftPosition at = vp.position();
		vp.tick(0.1);
		CHECK(vp.position().pan == at.pan);
		CHECK(vp.position().tilt == at.tilt);
		CHECK_FALSE(vp.moving());
	}
}

TEST_CASE("SoftViewport eased moves", "[motion]")
{
	SECTION("ease in and out of the target over the recall time")
	{
		SoftViewport vp = zoomedIn(1.0);
		vp.moveTo({0.5, 0, 1});
		CHECK(vp.moving());
		vp.tick(0.25);
		double quarter = vp.position().pan;
		CHECK(quarter > 0.0);
		CHECK(quarter < 0.25 * 0.5); /* slower than a straight line at first */
		vp.tick(0.25);
		CHECK(vp.position().pan == Approx(0.25).margin(0.01));
		vp.tick(0.5);
		CHECK(vp.position().pan == Approx(0.5));
		CHECK_FALSE(vp.moving());
	}

	SECTION("a retarget carries on from where it is")
	{
		SoftViewport vp = zoomedIn(1.0);
		vp.moveTo({0.7, 0, 1});
		vp.tick(0.3);
		double before = vp.position().pan;
		vp.moveTo({-0.5, 0, 1});
		vp.tick(0.016);
		CHECK(std::fabs(vp.position().pan - before) < 0.15);
		vp.tick(1.0);
		CHECK(vp.position().pan == Approx(-0.5));
	}

	SECTION("pan given before the zoom that makes room for it is kept")
	{
		/* What a preset recall does: pantilt_abs() then zoom_abs() */
		SoftViewport vp = zoomedIn(0.0);
		vp.movePanTiltTo(0.5, -0.25);
		vp.moveZoomTo(1.0);
		vp.tick(1.0);
		SoftPosition p = vp.position();
		CHECK(p.pan == Approx(0.5));
		CHECK(p.tilt == Approx(-0.25));
		CHECK(p.zoom == Approx(1.0));
		CHECK_FALSE(vp.moving());
	}

	SECTION("a long stall finishes the move, and no further")
	{
		SoftViewport vp = zoomedIn(1.0);
		vp.moveTo({0.5, -0.5, 0.75});
		vp.tick(5000);
		SoftPosition p = vp.position();
		CHECK(p.pan == Approx(0.5));
		CHECK(p.tilt == Approx(-0.5));
		CHECK(p.zoom == Approx(0.75));
	}

	SECTION("a zero recall time goes at once")
	{
		SoftViewport vp = zoomedIn(1.0);
		SoftViewportConfig config = vp.config();
		config.recallSeconds = 0;
		vp.setConfig(config);
		vp.moveTo({0.5, 0, 1});
		vp.tick(0.016);
		CHECK(vp.position().pan == Approx(0.5));
		CHECK_FALSE(vp.moving());
	}

	SECTION("a relative move is from where the viewport is shown")
	{
		SoftViewport vp = zoomedIn(1.0);
		vp.jumpTo({0.2, 0, 1});
		vp.moveBy(0.1, -0.1);
		vp.tick(1.0);
		CHECK(vp.position().pan == Approx(0.3));
		CHECK(vp.position().tilt == Approx(-0.1));
	}

	SECTION("nudges in quick succession add up")
	{
		/* A second nudge, one frame after the first, is from where the first
		 * is going to, not from where it has got to */
		SoftViewport vp = zoomedIn(1.0);
		vp.moveBy(0.1, 0);
		vp.tick(0.016);
		vp.moveBy(0.1, 0);
		vp.tick(5.0);
		CHECK(vp.position().pan == Approx(0.2));
	}

	SECTION("a nudge past the edge goes no further than the edge")
	{
		SoftViewport vp = zoomedIn(1.0);
		vp.jumpTo({0.7, 0, 1});
		vp.moveBy(0.2, 0);
		vp.tick(0.016);
		vp.moveBy(0.2, 0);
		vp.tick(5.0);
		CHECK(vp.position().pan == Approx(0.75));
	}

	SECTION("home is the whole frame")
	{
		SoftViewport vp = zoomedIn(1.0);
		vp.jumpTo({0.4, 0.4, 1});
		vp.home();
		vp.tick(1.0);
		SoftPosition p = vp.position();
		CHECK(p.pan == Approx(0.0));
		CHECK(p.tilt == Approx(0.0));
		CHECK(p.zoom == Approx(0.0));
	}
}

TEST_CASE("SoftViewport commands that take each other over", "[motion]")
{
	SECTION("a speed ends an eased move")
	{
		SoftViewport vp = zoomedIn(1.0);
		vp.moveTo({0.7, 0, 1});
		vp.tick(0.2);
		vp.setSpeeds(-1, 0, 0);
		double at = vp.position().pan;
		vp.tick(0.1);
		CHECK(vp.position().pan == Approx(at - 0.5 / 4 * 0.1));
		vp.tick(1.0);
		/* it never went on to 0.7 */
		CHECK(vp.position().pan < at);
	}

	SECTION("an eased move ends the speeds")
	{
		SoftViewport vp = zoomedIn(1.0);
		vp.setSpeeds(1, 0, 0);
		vp.moveTo({-0.25, 0, 1});
		vp.tick(1.0);
		CHECK(vp.position().pan == Approx(-0.25));
		vp.tick(0.1);
		CHECK(vp.position().pan == Approx(-0.25));
		CHECK_FALSE(vp.moving());
	}

	SECTION("a speed of nothing leaves an eased move alone")
	{
		SoftViewport vp = zoomedIn(1.0);
		vp.moveTo({0.5, 0, 1});
		vp.setSpeeds(0, 0, 0);
		CHECK(vp.moving());
		vp.tick(1.0);
		CHECK(vp.position().pan == Approx(0.5));
	}
}

TEST_CASE("SoftViewport setConfig and bad input", "[motion]")
{
	SECTION("a lower max zoom keeps the viewport in the frame")
	{
		SoftViewport vp = zoomedIn(1.0);
		vp.jumpTo({0.7, -0.7, 1});
		SoftViewportConfig config = vp.config();
		config.maxZoom = 2.0;
		vp.setConfig(config);
		SoftPosition p = vp.position();
		CHECK(p.pan <= 0.5 + 1e-9);
		CHECK(p.tilt >= -0.5 - 1e-9);
		SoftRect r = vp.visibleRect();
		CHECK(r.x >= -1e-9);
		CHECK(r.y >= -1e-9);
		CHECK(r.x + r.w <= 1.0 + 1e-9);
		CHECK(r.y + r.h <= 1.0 + 1e-9);
	}

	SECTION("a max zoom out of range is brought into it")
	{
		SoftViewport vp;
		SoftViewportConfig config;
		config.maxZoom = 100;
		vp.setConfig(config);
		CHECK(vp.config().maxZoom == Approx(10.0));
		config.maxZoom = 1.0;
		vp.setConfig(config);
		CHECK(vp.config().maxZoom == Approx(1.1));
	}

	SECTION("what is not a number is ignored")
	{
		const double nan = std::numeric_limits<double>::quiet_NaN();
		const double inf = std::numeric_limits<double>::infinity();
		SoftViewport vp = zoomedIn(1.0);
		vp.jumpTo({0.3, 0, 1});
		vp.moveTo({nan, 0, 0});
		vp.moveBy(nan, 0);
		vp.movePanTiltTo(inf, 0);
		vp.moveZoomTo(nan);
		vp.setSpeeds(inf, 0, 0);
		vp.tick(nan);
		CHECK_FALSE(vp.moving());
		CHECK(vp.position().pan == Approx(0.3));
		CHECK(vp.position().zoom == Approx(1.0));
	}
}
