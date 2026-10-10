/* Tests for PTZLifecycle (src/ptz-lifecycle.hpp): the order in which devices
 * are added, removed, started and stopped.
 *
 * Copyright 2026 Grant Likely <grant.likely@secretlab.ca>
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include <catch_amalgamated.hpp>
#include "ptz-lifecycle.hpp"

namespace {
struct Counter : PTZLifecycle::Listener {
	int starts = 0;
	int stops = 0;
	void onStartup() override { starts++; }
	void onShutdown() override { stops++; }
};
} // namespace

TEST_CASE("lifecycle: added_before_finish_starts_once", "[lifecycle]")
{
	PTZLifecycle lc;
	Counter c;
	lc.add(&c);
	REQUIRE(c.starts == 0);
	lc.loadingFinished();
	REQUIRE(c.starts == 1);
}

TEST_CASE("lifecycle: added_after_finish_starts_immediately", "[lifecycle]")
{
	PTZLifecycle lc;
	lc.loadingFinished();
	Counter c;
	lc.add(&c);
	REQUIRE(c.starts == 1);
	lc.loadingFinished();
	REQUIRE(c.starts == 1);
}

TEST_CASE("lifecycle: added_during_finish_starts_once", "[lifecycle]")
{
	/* Made from inside another listener's onStartup(), the way legacy
	 * migration makes a device while FINISHED_LOADING is dispatched */
	struct Maker : Counter {
		PTZLifecycle *lc;
		Counter *made;
		void onStartup() override
		{
			Counter::onStartup();
			lc->add(made);
		}
	};
	PTZLifecycle lc;
	Counter made;
	Maker maker;
	maker.lc = &lc;
	maker.made = &made;
	lc.add(&maker);
	lc.loadingFinished();
	REQUIRE(made.starts == 1);
	REQUIRE(maker.starts == 1);
}

TEST_CASE("lifecycle: removed_before_finish_never_starts", "[lifecycle]")
{
	PTZLifecycle lc;
	Counter c;
	lc.add(&c);
	lc.remove(&c);
	lc.loadingFinished();
	lc.exiting();
	REQUIRE(c.starts == 0);
	REQUIRE(c.stops == 0);
}

TEST_CASE("lifecycle: exit_delivers_once_to_live_listeners", "[lifecycle]")
{
	PTZLifecycle lc;
	Counter live, gone;
	lc.add(&live);
	lc.add(&gone);
	lc.loadingFinished();
	lc.remove(&gone);
	lc.exiting();
	lc.exiting();
	REQUIRE(live.stops == 1);
	REQUIRE(gone.stops == 0);
}

TEST_CASE("lifecycle: finish_twice_starts_once", "[lifecycle]")
{
	PTZLifecycle lc;
	Counter c;
	lc.add(&c);
	lc.loadingFinished();
	lc.loadingFinished();
	REQUIRE(c.starts == 1);
}
