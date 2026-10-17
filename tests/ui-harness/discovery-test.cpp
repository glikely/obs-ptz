/* PTZ UI test harness: the drivers' discovery kinds, driven only through
 * their instances' own procs and signals, as the Add Device dialog does
 *
 * Copyright 2026 Grant Likely <grant.likely@secretlab.ca>
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include "ui-test-harness.hpp"

#include <atomic>
#include <deque>
#include <memory>
#include <vector>
#include <obs-module.h>
#include <obs.hpp>
#include <QTimer>

namespace {

void writeResult(obs_data_t *result, const QString &filename, const char *test)
{
	if (!obs_data_save_json_safe(result, qUtf8Printable(filename), "tmp", "bak"))
		blog(LOG_INFO, "[ptz-ui-test] %s: failed to write %s", test, qUtf8Printable(filename));
}

/* --- discover: one instance of each kind, until each has finished --------- */

struct Search;

struct SearchInstance {
	Search *search;
	QString kind;
	OBSSource source;
	long long major = -1;
	long long minor = -1;
	int finished = 0;
	int foundAfterFinished = 0;
	/* With stop_on_found: whether it was stopped, and what it found after */
	bool stopped = false;
	int foundAfterStop = 0;
	/* With release_on_found: whether its last reference was let go from
	 * inside a report, and what was said after */
	bool released = false;
	int foundAfterRelease = 0;
	int finishedAfterRelease = 0;
};

struct Search {
	QString filename;
	bool stopOnFound = false;
	bool restartOnFinished = false;
	bool releaseOnFound = false;
	std::vector<std::unique_ptr<SearchInstance>> instances;
	OBSDataArrayAutoRelease found = obs_data_array_create();
	bool written = false;
	/* The context of its timers, which go with it */
	QObject guard;
};

void searchFoundCb(void *data, calldata_t *cd);
void searchFinishedCb(void *data, calldata_t *cd);

void checkSearchFinished(Search *search);

void callProc(obs_source_t *source, const char *proc)
{
	calldata_t cd = {};
	proc_handler_call(obs_source_get_proc_handler(source), proc, &cd);
	calldata_free(&cd);
}

void searchFoundCb(void *data, calldata_t *cd)
{
	auto instance = static_cast<SearchInstance *>(data);
	OBSDataAutoRelease item = obs_data_create();
	obs_data_set_string(item, "discovery", qUtf8Printable(instance->kind));
	for (const char *key : {"kind", "id", "name", "address"}) {
		const char *value = calldata_string(cd, key);
		obs_data_set_string(item, key, value ? value : "");
	}
	OBSDataAutoRelease settings = obs_data_create();
	if (auto lent = static_cast<obs_data_t *>(calldata_ptr(cd, "settings")))
		obs_data_apply(settings, lent);
	obs_data_set_obj(item, "settings", settings);
	obs_data_set_bool(item, "source_is_instance", calldata_ptr(cd, "source") == instance->source.Get());
	obs_data_array_push_back(instance->search->found, item);
	if (instance->finished)
		instance->foundAfterFinished++;
	if (instance->stopped)
		instance->foundAfterStop++;
	if (instance->released) {
		instance->foundAfterRelease++;
		return;
	}
	/* The harness holds the one reference but the signal's own: let it go
	 * from inside the signal, as a controller may */
	if (instance->search->releaseOnFound) {
		instance->released = true;
		instance->source = nullptr;
		checkSearchFinished(instance->search);
		return;
	}
	/* From inside the signal, as a controller may */
	if (instance->search->stopOnFound && !instance->stopped) {
		instance->stopped = true;
		callProc(instance->source, "ptz_discovery_stop");
	}
}

/* Writes what each instance said, then lets them go */
void finishSearch(Search *search)
{
	if (search->written)
		return;
	search->written = true;
	OBSDataAutoRelease instances = obs_data_create();
	for (const auto &instance : search->instances) {
		/* A released one's handler goes with it */
		if (instance->source) {
			signal_handler_t *sh = obs_source_get_signal_handler(instance->source);
			signal_handler_disconnect(sh, "ptz_device_found", searchFoundCb, instance.get());
			signal_handler_disconnect(sh, "ptz_discovery_finished", searchFinishedCb, instance.get());
		}
		OBSDataAutoRelease item = obs_data_create();
		obs_data_set_int(item, "major", instance->major);
		obs_data_set_int(item, "minor", instance->minor);
		obs_data_set_int(item, "finished", instance->finished);
		obs_data_set_int(item, "found_after_finished", instance->foundAfterFinished);
		obs_data_set_bool(item, "stopped", instance->stopped);
		obs_data_set_int(item, "found_after_stop", instance->foundAfterStop);
		obs_data_set_bool(item, "released", instance->released);
		obs_data_set_int(item, "found_after_release", instance->foundAfterRelease);
		obs_data_set_int(item, "finished_after_release", instance->finishedAfterRelease);
		obs_data_set_obj(instances, qUtf8Printable(instance->kind), item);
	}
	OBSDataAutoRelease result = obs_data_create();
	obs_data_set_obj(result, "instances", instances);
	obs_data_set_array(result, "found", search->found);
	writeResult(result, search->filename, "discover");
	/* Not from inside one of its own timers */
	QTimer::singleShot(0, [search]() { delete search; });
}

void searchFinishedCb(void *data, calldata_t *)
{
	auto instance = static_cast<SearchInstance *>(data);
	if (instance->released) {
		instance->finishedAfterRelease++;
		return;
	}
	instance->finished++;
	Search *search = instance->search;
	/* From inside the signal, as a controller may: look again, once */
	if (search->restartOnFinished && instance->finished == 1) {
		callProc(instance->source, "ptz_discovery_start");
		return;
	}
	checkSearchFinished(search);
}

/* Done once each instance has finished, or was let go */
void checkSearchFinished(Search *search)
{
	int searches = search->restartOnFinished ? 2 : 1;
	for (const auto &other : search->instances)
		if (!other->released && other->finished < searches)
			return;
	/* A moment more, for anything said after finishing, and not from
	 * inside the signal */
	QTimer::singleShot(300, &search->guard, [search]() { finishSearch(search); });
}

void runDiscoverReleasedTest(const QString &filename, int waitMs);

/* Runs on the main thread, as the drivers' signals do */
void runDiscoverTest(const QMap<QString, QString> &params)
{
	if (params.value(QStringLiteral("filename")).isEmpty()) {
		blog(LOG_INFO, "[ptz-ui-test] discover: missing filename");
		return;
	}
	if (params.value(QStringLiteral("release_while_running")) == QStringLiteral("true")) {
		runDiscoverReleasedTest(params.value(QStringLiteral("filename")),
					params.value(QStringLiteral("wait_ms"), QStringLiteral("4000")).toInt());
		return;
	}
	auto search = new Search;
	search->filename = params.value(QStringLiteral("filename"));
	search->stopOnFound = params.value(QStringLiteral("stop_on_found")) == QStringLiteral("true");
	search->restartOnFinished = params.value(QStringLiteral("restart_on_finished")) == QStringLiteral("true");
	search->releaseOnFound = params.value(QStringLiteral("release_on_found")) == QStringLiteral("true");
	for (const QString &kind : ptzUITestDiscoveryKinds()) {
		OBSSourceAutoRelease source = obs_source_create_private(qUtf8Printable(kind), "ptz-ui-test", nullptr);
		if (!source)
			continue;
		auto instance = std::make_unique<SearchInstance>();
		instance->search = search;
		instance->kind = kind;
		instance->source = source.Get();
		calldata_t cd = {};
		if (proc_handler_call(obs_source_get_proc_handler(source), "ptz_get_api_version", &cd)) {
			instance->major = calldata_int(&cd, "major");
			instance->minor = calldata_int(&cd, "minor");
		}
		calldata_free(&cd);
		signal_handler_t *sh = obs_source_get_signal_handler(source);
		signal_handler_connect(sh, "ptz_device_found", searchFoundCb, instance.get());
		signal_handler_connect(sh, "ptz_discovery_finished", searchFinishedCb, instance.get());
		search->instances.push_back(std::move(instance));
	}
	/* Each driver gives up within seconds; this is for one that doesn't */
	QTimer::singleShot(15000, &search->guard, [search]() { finishSearch(search); });
	for (const auto &instance : search->instances)
		callProc(instance->source, "ptz_discovery_start");
	if (search->instances.empty())
		finishSearch(search);
}

/* --- discover with release_while_running ------------------------------------ */

/* What one released instance said after it was released, and whether OBS
 * said "destroy" for it (which libobs signals just before calling the
 * kind's destroy, so it says destroying has begun, not that it is done) */
struct ReleasedInstance {
	std::atomic<int> found{0};
	std::atomic<int> finished{0};
	std::atomic<int> foundAfterDestroy{0};
	std::atomic<int> finishedAfterDestroy{0};
	std::atomic<bool> destroyed{false};
};
/* Kept until the next such test: a broken destroy could still signal into
 * one after its result is written */
std::deque<ReleasedInstance> released;

void releasedFoundCb(void *data, calldata_t *)
{
	auto r = static_cast<ReleasedInstance *>(data);
	r->found++;
	if (r->destroyed)
		r->foundAfterDestroy++;
}

void releasedFinishedCb(void *data, calldata_t *)
{
	auto r = static_cast<ReleasedInstance *>(data);
	r->finished++;
	if (r->destroyed)
		r->finishedAfterDestroy++;
}

void releasedDestroyCb(void *data, calldata_t *)
{
	static_cast<ReleasedInstance *>(data)->destroyed = true;
}

/* Starts an instance of each kind and lets it go at once, without stopping
 * it, then reports, after the drivers' own searches would have run their
 * course, what reached a listener after that */
void runDiscoverReleasedTest(const QString &filename, int waitMs)
{
	released.clear();
	for (const QString &kind : ptzUITestDiscoveryKinds()) {
		OBSSourceAutoRelease source = obs_source_create_private(qUtf8Printable(kind), "ptz-ui-test", nullptr);
		if (!source)
			continue;
		ReleasedInstance &r = released.emplace_back();
		signal_handler_t *sh = obs_source_get_signal_handler(source);
		callProc(source, "ptz_discovery_start");
		/* Connected after the start, and released below with nothing run in
		 * between, so all it hears comes after the release */
		signal_handler_connect(sh, "ptz_device_found", releasedFoundCb, &r);
		signal_handler_connect(sh, "ptz_discovery_finished", releasedFinishedCb, &r);
		signal_handler_connect(sh, "destroy", releasedDestroyCb, &r);
	}
	QTimer::singleShot(waitMs, [filename]() {
		int found = 0, finished = 0, foundAfterDestroy = 0, finishedAfterDestroy = 0, destroyed = 0;
		for (const auto &r : released) {
			found += r.found;
			finished += r.finished;
			foundAfterDestroy += r.foundAfterDestroy;
			finishedAfterDestroy += r.finishedAfterDestroy;
			destroyed += r.destroyed ? 1 : 0;
		}
		OBSDataAutoRelease result = obs_data_create();
		obs_data_set_int(result, "instances", (long long)released.size());
		obs_data_set_int(result, "destroyed", destroyed);
		obs_data_set_int(result, "found_after_release", found);
		obs_data_set_int(result, "finished_after_release", finished);
		obs_data_set_int(result, "found_after_destroy", foundAfterDestroy);
		obs_data_set_int(result, "finished_after_destroy", finishedAfterDestroy);
		writeResult(result, filename, "discover");
	});
}

/* --- the spy on the add dialog's instances --------------------------------- */

/* Counts, with no reference held, what the instances it was armed on said,
 * and how many OBS has begun to destroy: "destroy" is libobs's signal, sent
 * on its own thread just before it calls the kind's destroy, so it says
 * destroying has begun, not that it has finished */
struct DiscoverySpy {
	std::atomic<int> instances{0};
	std::atomic<int> found{0};
	std::atomic<int> finished{0};
	std::atomic<int> destroyed{0};
	int foundAtClose = 0;
	int finishedAtClose = 0;
	bool closed = false;
};
DiscoverySpy spy;

void spyFoundCb(void *, calldata_t *)
{
	spy.found++;
}

void spyFinishedCb(void *, calldata_t *)
{
	spy.finished++;
}

void spyDestroyCb(void *, calldata_t *)
{
	spy.destroyed++;
}

void runGetDiscoverySpyTest(const QMap<QString, QString> &params)
{
	QString filename = params.value(QStringLiteral("filename"));
	if (filename.isEmpty())
		return;
	OBSDataAutoRelease result = obs_data_create();
	obs_data_set_int(result, "instances", spy.instances);
	obs_data_set_int(result, "destroyed", spy.destroyed);
	obs_data_set_int(result, "alive", spy.instances - spy.destroyed);
	obs_data_set_int(result, "found", spy.found);
	obs_data_set_int(result, "finished", spy.finished);
	obs_data_set_bool(result, "closed", spy.closed);
	obs_data_set_int(result, "found_after_close", spy.closed ? spy.found - spy.foundAtClose : 0);
	obs_data_set_int(result, "finished_after_close", spy.closed ? spy.finished - spy.finishedAtClose : 0);
	writeResult(result, filename, "get_discovery_spy");
}

} // namespace

static void armDiscoverySpy(const QMap<QString, QString> &)
{
	spy.instances = 0;
	spy.found = 0;
	spy.finished = 0;
	spy.destroyed = 0;
	spy.closed = false;
	QStringList kinds = ptzUITestDiscoveryKinds();
	auto cb = [](void *data, obs_source_t *source) {
		auto kinds = static_cast<QStringList *>(data);
		if (!kinds->contains(QString::fromUtf8(obs_source_get_id(source))))
			return true;
		/* Not the dialog's, but the ones the websocket binding keeps for its
		 * clients (ptz_list_discovery), which outlive any dialog */
		if (strcmp(obs_source_get_name(source), "ptz-websocket") == 0)
			return true;
		signal_handler_t *sh = obs_source_get_signal_handler(source);
		signal_handler_connect(sh, "ptz_device_found", spyFoundCb, nullptr);
		signal_handler_connect(sh, "ptz_discovery_finished", spyFinishedCb, nullptr);
		signal_handler_connect(sh, "destroy", spyDestroyCb, nullptr);
		spy.instances++;
		return true;
	};
	obs_enum_all_sources(cb, &kinds);
}

static void closeDiscoverySpy(const QMap<QString, QString> &)
{
	spy.foundAtClose = spy.found;
	spy.finishedAtClose = spy.finished;
	spy.closed = true;
}

/* discover: filename - where to write, once an instance of every discovery
 *   kind has said ptz_discovery_finished (or after 15s), {"instances": {kind:
 *   {"major", "minor" (from its ptz_get_api_version), "finished" (how often it
 *   said so), "found_after_finished"}}, "found": [{"discovery" (the instance's
 *   kind), "kind", "id", "name", "address", "settings", "source_is_instance"}...]}.
 *   stop_on_found: "true" to call ptz_discovery_stop from inside an instance's
 *   first ptz_device_found (each instance then also has "stopped" and
 *   "found_after_stop"); restart_on_finished: "true" to call
 *   ptz_discovery_start from inside its first ptz_discovery_finished, and wait
 *   for a second. release_on_found: "true" to let go of the harness's
 *   reference, the instance's last but the signal's own, from inside its
 *   first ptz_device_found (then "released", "found_after_release" and
 *   "finished_after_release"). release_while_running: "true" to start an instance of each
 *   kind and release it at once without stopping it, then after wait_ms
 *   (default 4000) write {"instances", "destroyed", "found_after_release",
 *   "finished_after_release", "found_after_destroy", "finished_after_destroy"}
 * get_discovery_spy: filename - where to write {"instances", "destroyed",
 *   "alive", "found", "finished", "closed", "found_after_close",
 *   "finished_after_close"}: what the add dialog's discovery instances did,
 *   counted from arm_discovery_spy
 * arm_discovery_spy: start counting what the discovery instances that exist now
 *   say, and whether OBS destroys them (the add dialog's, while it is open)
 * close_discovery_spy: mark the moment the add dialog closed
 */
void registerDiscoveryTest(PTZUITestHarness *harness)
{
	harness->registerTest(QStringLiteral("discover"), &runDiscoverTest);
	harness->registerTest(QStringLiteral("get_discovery_spy"), &runGetDiscoverySpyTest);
	harness->registerTest(QStringLiteral("arm_discovery_spy"), &armDiscoverySpy);
	harness->registerTest(QStringLiteral("close_discovery_spy"), &closeDiscoverySpy);
}
