/* PTZ UI test harness: a reference held to a device's source, as a controller
 * may hold one
 *
 * Copyright 2026 Grant Likely <grant.likely@secretlab.ca>
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include "ui-test-harness.hpp"

#include <obs.hpp>
#include <obs-module.h>

#include <mutex>
#include <vector>

namespace {

/* Strong references, as an NDI-style controller keeps to the sources it uses:
 * OBS destroys none of them, and says no "destroy", until they are let go */
std::mutex heldLock;
std::vector<OBSSource> held;

/* The source named "source", or, with "filter", the filter of that name on it */
void runHoldSourceTest(const QMap<QString, QString> &params)
{
	OBSSourceAutoRelease source = obs_get_source_by_name(qUtf8Printable(params.value(QStringLiteral("source"))));
	OBSSourceAutoRelease filter;
	if (source && params.contains(QStringLiteral("filter")))
		filter = obs_source_get_filter_by_name(source, qUtf8Printable(params.value(QStringLiteral("filter"))));
	obs_source_t *hold = params.contains(QStringLiteral("filter")) ? filter.Get() : source.Get();
	if (!hold) {
		blog(LOG_INFO, "[ptz-ui-test] hold_source: no such source");
		return;
	}
	std::lock_guard<std::mutex> lock(heldLock);
	held.push_back(hold);
}

/* Puts the filter hold_source last held back on the source named "source": the
 * same filter, as only a plugin can, and not a new one made from its settings */
void runAddHeldFilterTest(const QMap<QString, QString> &params)
{
	OBSSourceAutoRelease source = obs_get_source_by_name(qUtf8Printable(params.value(QStringLiteral("source"))));
	OBSSource filter;
	{
		std::lock_guard<std::mutex> lock(heldLock);
		if (!held.empty())
			filter = held.back();
	}
	if (!source || !filter || obs_source_get_type(filter) != OBS_SOURCE_TYPE_FILTER) {
		blog(LOG_INFO, "[ptz-ui-test] add_held_filter: no such source, or no filter held");
		return;
	}
	obs_source_filter_add(source, filter);
}

/* Lets go of every reference hold_source took, and waits for OBS to destroy
 * what that was the last of, so that its "destroy" has been said by the answer */
void runReleaseHeldTest(const QMap<QString, QString> &)
{
	{
		std::vector<OBSSource> releasing;
		std::lock_guard<std::mutex> lock(heldLock);
		releasing.swap(held);
	}
	obs_wait_for_destroy_queue();
}

} // namespace

/* hold_source request params:
 *   source - the name of the source to hold
 *   filter - the name of a filter on it, to hold that instead
 * add_held_filter request params:
 *   source - the name of the source to put the held filter on
 * release_held: no params */
void registerDeviceLifecycleTest(PTZUITestHarness *harness)
{
	harness->registerInlineTest(QStringLiteral("hold_source"), &runHoldSourceTest);
	harness->registerInlineTest(QStringLiteral("add_held_filter"), &runAddHeldFilterTest);
	harness->registerInlineTest(QStringLiteral("release_held"), &runReleaseHeldTest);
}
