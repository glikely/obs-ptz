/* obs-ui-driver: lets a client of obs-websocket find and drive OBS's Qt
 * widgets
 *
 * Copyright 2026 Grant Likely <grant.likely@secretlab.ca>
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * Its requests are on obs-websocket's vendor "ui-driver" (see README.md for
 * them). Each is given its data, and answers, as one string field, "json",
 * holding a JSON object: obs-websocket hands a vendor obs_data, which can't
 * hold an array of plain values.
 *
 * Threads. obs-websocket calls a vendor's request on a thread of its own.
 * What a request does with Qt objects is done on the main thread, which the
 * request waits for, for no longer than kMainThreadWait, and not at all once
 * OBS has said it is exiting: from then on the main thread is taking the UI
 * apart, and nothing more is started.
 */

#include <obs-frontend-api.h>
#include <obs-module.h>
#include <obs.hpp>

#include <QApplication>
#include <QJsonDocument>
#include <QJsonObject>
#include <QThread>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <functional>
#include <list>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "obs-websocket-api.h"
#include "ui-driver-objects.hpp"
#include "ui-driver.hpp"

OBS_DECLARE_MODULE()

MODULE_EXPORT const char *obs_module_description(void)
{
	return "Lets a client of obs-websocket find and drive OBS's Qt widgets, for tests";
}

QJsonObject error(const char *code, const QString &message)
{
	return QJsonObject{{"error", code}, {"message", message}};
}

namespace {

constexpr auto kMainThreadWait = std::chrono::seconds(5);
constexpr auto kUnloadWait = std::chrono::seconds(1);

obs_websocket_vendor vendor = nullptr;

struct Request {
	std::string name;
	Handler handler;
	bool registered = false;
};

/* Kept for as long as the module is loaded: obs-websocket holds a pointer to each */
std::list<Request> requests;

/* The sources hold_source keeps, till the module unloads: as a plugin with an
 * output on a scene does, which keeps the sources in it past OBS clearing its
 * scenes at exit. Main thread. */
std::vector<OBSSource> heldSources;

/* OBS is exiting, or the module unloading: nothing more is started */
std::atomic<bool> closing{false};
/* The requests that are in this code now, which unloading waits for */
std::atomic<int> inFlight{0};

struct InFlight {
	InFlight() { inFlight++; }
	~InFlight() { inFlight--; }
};

/* Guards `done` and `cancelled` of every Hop, and is what a waiter waits on */
std::mutex hopMutex;
std::condition_variable hopChanged;

struct Hop {
	bool done = false;
	bool cancelled = false;
	QJsonObject answer;
};

/* Runs `handler` on the main thread and waits for its answer, but no longer
 * than kMainThreadWait, and not at all once OBS is exiting. It isn't run at
 * all if the caller gave up before the main thread got to it. */
QJsonObject onMainThread(const Handler &handler, const QJsonObject &request)
{
	if (closing.load())
		return error("unavailable", "OBS is exiting");
	if (QThread::currentThread() == qApp->thread())
		return handler(request);

	auto hop = std::make_shared<Hop>();
	QMetaObject::invokeMethod(
		qApp,
		[hop, handler, request]() {
			{
				/* Not once OBS has said it is exiting, although the request
				 * hasn't woken to give up yet: OBS runs what was posted while
				 * it takes the UI apart */
				std::lock_guard<std::mutex> lock(hopMutex);
				if (hop->cancelled || closing.load())
					return;
			}
			QJsonObject answer = handler(request);
			{
				std::lock_guard<std::mutex> lock(hopMutex);
				hop->answer = answer;
				hop->done = true;
			}
			hopChanged.notify_all();
		},
		Qt::QueuedConnection);

	std::unique_lock<std::mutex> lock(hopMutex);
	hopChanged.wait_for(lock, kMainThreadWait, [&]() { return hop->done || closing.load(); });
	if (hop->done)
		return hop->answer;
	hop->cancelled = true;
	if (closing.load())
		return error("unavailable", "OBS is exiting");
	return error("timeout", "OBS's main thread did not get to the request in time");
}

void requestCallback(obs_data_t *requestData, obs_data_t *responseData, void *priv)
{
	InFlight running;
	auto request = static_cast<Request *>(priv);

	QJsonObject answer;
	const char *text = obs_data_get_string(requestData, "json");
	QJsonDocument document = QJsonDocument::fromJson(*text ? QByteArray(text) : QByteArray("{}"));
	if (!document.isObject())
		answer = error("bad_request", "the request's \"json\" is not a JSON object");
	else
		answer = onMainThread(request->handler, document.object());

	obs_data_set_string(responseData, "json", QJsonDocument(answer).toJson(QJsonDocument::Compact).constData());
}

/* Stops starting anything, and wakes whatever is waiting for the main thread */
void beginClosing()
{
	{
		std::lock_guard<std::mutex> lock(hopMutex);
		closing.store(true);
	}
	hopChanged.notify_all();
}

/* obs-websocket is still there when OBS says it is exiting, and isn't once
 * modules unload, so it is let go of now */
void letGoOfWebsocket()
{
	beginClosing();
	stopWatchingForSecrets();
	for (Request &request : requests) {
		if (request.registered)
			obs_websocket_vendor_unregister_request(vendor, request.name.c_str());
		request.registered = false;
	}
}

void frontendEvent(enum obs_frontend_event event, void *)
{
	if (event == OBS_FRONTEND_EVENT_EXIT)
		letGoOfWebsocket();
}

} // namespace

void addRequest(const char *name, Handler handler)
{
	Request &request = requests.emplace_back(Request{name, std::move(handler)});
	request.registered = obs_websocket_vendor_register_request(vendor, name, requestCallback, &request);
	if (!request.registered)
		blog(LOG_WARNING, "[obs-ui-driver] could not register the request %s", name);
}

bool obs_module_load(void)
{
	return true;
}

void obs_module_post_load(void)
{
	if (qgetenv("OBS_UI_DRIVER") != "1")
		return;
	/* obs-websocket has to have loaded first, which it has by post_load */
	vendor = obs_websocket_register_vendor("ui-driver");
	if (!vendor) {
		blog(LOG_WARNING, "[obs-ui-driver] no obs-websocket, or its vendor \"ui-driver\" is taken");
		return;
	}
	watchForSecrets();
	addRequest("ping", [](const QJsonObject &) { return QJsonObject{{"driver", 1}}; });
	addRequest("hold_source", [](const QJsonObject &request) {
		OBSSourceAutoRelease source = obs_get_source_by_name(request["name"].toString().toUtf8().constData());
		if (!source)
			return error("not_found", QString("no source is named %1").arg(request["name"].toString()));
		heldSources.emplace_back(source.Get());
		return QJsonObject{};
	});
	addReadingRequests();
	addActionRequests();
	obs_frontend_add_event_callback(frontendEvent, nullptr);
	/* If OBS never says it is exiting, it is still about to quit before it unloads modules */
	QObject::connect(qApp, &QCoreApplication::aboutToQuit, qApp, []() { beginClosing(); });
	blog(LOG_INFO, "[obs-ui-driver] answering on obs-websocket's vendor \"ui-driver\"");
}

void obs_module_unload(void)
{
	if (!vendor)
		return;
	obs_frontend_remove_event_callback(frontendEvent, nullptr);
	beginClosing();
	stopWatchingForSecrets();
	/* The requests that have started leave this code before it goes */
	auto deadline = std::chrono::steady_clock::now() + kUnloadWait;
	while (inFlight.load() > 0 && std::chrono::steady_clock::now() < deadline)
		std::this_thread::sleep_for(std::chrono::milliseconds(5));
	if (inFlight.load() > 0)
		blog(LOG_WARNING, "[obs-ui-driver] %d requests were still running at unload", inFlight.load());
	heldSources.clear();
}
