/* SoftViewport: where the viewport is, and what part of the frame it shows.
 *
 * SPDX-License-Identifier: GPLv2
 */
#include <catch_amalgamated.hpp>

#include "ptz-soft-viewport.hpp"

using Catch::Approx;

TEST_CASE("SoftViewport position", "[viewport]")
{
	SoftViewport vp;

	SECTION("starts at the whole frame")
	{
		SoftPosition p = vp.position();
		CHECK(p.pan == 0.0);
		CHECK(p.tilt == 0.0);
		CHECK(p.zoom == 0.0);
		CHECK(vp.scale() == Approx(1.0));
		SoftRect r = vp.visibleRect();
		CHECK(r.x == Approx(0.0));
		CHECK(r.y == Approx(0.0));
		CHECK(r.w == Approx(1.0));
		CHECK(r.h == Approx(1.0));
	}

	SECTION("full zoom shows the centre at 1/max_zoom of the frame")
	{
		vp.jumpTo({0, 0, 1});
		CHECK(vp.scale() == Approx(4.0));
		SoftRect r = vp.visibleRect();
		CHECK(r.w == Approx(0.25));
		CHECK(r.h == Approx(0.25));
		CHECK(r.x == Approx(0.375));
		CHECK(r.y == Approx(0.375));
	}

	SECTION("zoom maps exponentially to scale")
	{
		vp.jumpTo({0, 0, 0.5});
		CHECK(vp.scale() == Approx(2.0));
	}

	SECTION("pan and tilt clamp so the viewport stays in the frame")
	{
		vp.jumpTo({1.0, -1.0, 1.0});
		SoftPosition p = vp.position();
		CHECK(p.pan == Approx(0.75));
		CHECK(p.tilt == Approx(-0.75));
		SoftRect r = vp.visibleRect();
		CHECK(r.x == Approx(0.75));
		CHECK(r.x + r.w == Approx(1.0));
		/* tilt down is a larger y: the frame's origin is its top left */
		CHECK(r.y == Approx(0.75));
		CHECK(r.y + r.h == Approx(1.0));
	}

	SECTION("there is no pan or tilt range at zoom 0")
	{
		vp.jumpTo({0.5, 0.5, 0.0});
		CHECK(vp.position().pan == Approx(0.0));
		CHECK(vp.position().tilt == Approx(0.0));
	}

	SECTION("tilt up moves the viewport up the frame")
	{
		vp.jumpTo({0, 0.5, 1});
		CHECK(vp.visibleRect().y < 0.375);
		/* and pan right moves it right */
		vp.jumpTo({0.5, 0, 1});
		CHECK(vp.visibleRect().x > 0.375);
	}
}
