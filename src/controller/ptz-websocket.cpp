/* Pan Tilt Zoom Controls - the PTZ API over obs-websocket
 *
 * Copyright 2026 Grant Likely <grant.likely@secretlab.ca>
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * See "WebSocket binding" in docs/ptz-device-api.md for what a client sees.
 *
 * A request of the vendor is a proc of the PTZ API by the same name, on the
 * device whose UUID the request gives. Nothing here knows the API's procs: a
 * device says what it has in ptz_get_api_info, with the declaration of each,
 * and a request's JSON is turned into the proc's calldata by that.
 *
 * Threads. obs-websocket calls a request on any thread of its own pool, and
 * several at once. A request holds a reference to its device for as long as it
 * runs and shares nothing with another, so needs no lock. What is shared is the
 * list of the requests there are (`requests`), which only the main thread
 * touches: obs-websocket keeps its own copy of what is registered, and hands a
 * request the entry it was registered with, which is never freed.
 *
 * A proc that has to be called on the main thread is sent there and waited for,
 * for no longer than kMainThreadWait: see onMainThread(). What it works on is
 * its own (a ProcCall), kept by whichever of the two threads is last to let go
 * of it, so that a request that gave up waiting leaves nothing behind for the
 * main thread to write to. No lock is held while a proc, or OBS, is called.
 *
 * Signals. Each signal a device says it has is connected to, and sent on as an
 * event of the vendor, on whatever thread raised it: its calldata is read
 * and sent before the handler returns, and nothing of it is kept. Which
 * sources are connected to (`connected`) has a lock of its own, held only to
 * look one up or change the list.
 *
 * Closing. From the moment OBS says it is closing, a request is answered
 * "unavailable" and one that is waiting gives up, so that nothing waits for a
 * main thread that has stopped running what it is sent; unloading then waits,
 * for no longer than kUnloadWait, for the requests still running to leave this
 * code.
 */
#include "ptz-websocket.hpp"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstring>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <thread>
#include <vector>

#include <obs.hpp>
#include <obs-module.h>
#include <obs-frontend-api.h>
#include <QCoreApplication>
#include <QMetaObject>
#include <QThread>
#include <qt-wrappers.hpp>

#include "ptz.h"
#include "ptz-controls.hpp"
#include "ptz-driver-kinds.hpp"
#include "ptz-ws-decl.hpp"

namespace {

/* The version of the binding, which can change while the API's doesn't */
constexpr int kBridgeVersionMajor = 1;
constexpr int kBridgeVersionMinor = 0;

/* What a request gives as its "device" to mean the camera selected in the dock */
constexpr const char *kSelectedDevice = "selected";

/* How long a request waits for the main thread to run its proc */
constexpr auto kMainThreadWait = std::chrono::seconds(5);
/* How long unloading waits for the requests that are running to finish */
constexpr auto kUnloadWait = std::chrono::seconds(5);

enum class Access { Off, ReadOnly, Control };

/* Null before obs-websocket has given it, and once it has been given back */
std::atomic<obs_websocket_vendor> vendor{nullptr};
/* ptz_websocket_load() has run, with an obs-websocket to load for. Main thread. */
bool loaded = false;
std::atomic<Access> accessLevel{Access::Control};

/* A request of the vendor's. `access` is what a client has to be let do for
 * it to be there: "read" or "control". */
struct Request {
	std::string name;
	std::string access;
	obs_websocket_request_callback_function callback;
	bool registered = false;
};

/* Every request there has been cause to have, by name. Main thread. An entry
 * is never removed, as obs-websocket may be about to call it. */
std::map<std::string, std::unique_ptr<Request>> requests;

/* The names of the requests and events there are for the API, by the scope
 * docs/ptz-device-api.md has them in ("device-proc", ...). Main thread. */
std::map<std::string, std::set<std::string>> exposed;

/* The UUID of the camera selected in the dock, "" for none. Set on the main
 * thread as the selection changes, and read by a request on any. */
std::mutex selectedMutex;
std::string selectedUuid;

std::string selectedDevice()
{
	std::lock_guard<std::mutex> lock(selectedMutex);
	return selectedUuid;
}

/* The discovery instances made for clients, by kind: see listDiscovery(). Main thread. */
std::map<std::string, OBSSource> discoveryInstances;

bool admits(Access level, const std::string &access)
{
	if (level == Access::Control)
		return access == "read" || access == "control";
	if (level == Access::ReadOnly)
		return access == "read";
	return false;
}

void setError(obs_data_t *response, const char *code, const std::string &message)
{
	obs_data_set_string(response, "error", code);
	obs_data_set_string(response, "message", message.c_str());
}

/* OBS is closing, or the plugin unloading: nothing more is started */
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
};

enum class HopResult { Done, Timeout, Unavailable };

/* Runs `work` on the main thread and waits for it, but no longer than
 * kMainThreadWait, and not at all once OBS is closing. `work` owns everything
 * it touches (it is copied to the main thread, and may run, or finish, after
 * the caller has given up), and is not run at all if the caller gave up before
 * the main thread got to it. */
HopResult onMainThread(const std::function<void()> &work)
{
	if (closing.load())
		return HopResult::Unavailable;
	if (QThread::currentThread() == qApp->thread()) {
		work();
		return HopResult::Done;
	}
	auto hop = std::make_shared<Hop>();
	QMetaObject::invokeMethod(
		qApp,
		[hop, work]() {
			{
				std::lock_guard<std::mutex> lock(hopMutex);
				if (hop->cancelled)
					return;
			}
			work();
			{
				std::lock_guard<std::mutex> lock(hopMutex);
				hop->done = true;
			}
			hopChanged.notify_all();
		},
		Qt::QueuedConnection);

	std::unique_lock<std::mutex> lock(hopMutex);
	hopChanged.wait_for(lock, kMainThreadWait, [&]() { return hop->done || closing.load(); });
	if (hop->done)
		return HopResult::Done;
	hop->cancelled = true;
	return closing.load() ? HopResult::Unavailable : HopResult::Timeout;
}

/* The answer to a request that the main thread didn't get to. True if it did. */
bool hopDone(HopResult result, const std::string &what, obs_data_t *response)
{
	if (result == HopResult::Timeout)
		setError(response, "timeout", what + " was not run in time: OBS's main thread is busy");
	else if (result == HopResult::Unavailable)
		setError(response, "unavailable", "OBS is closing");
	return result == HopResult::Done;
}

/* --- what a device says it has ------------------------------------------------- */

struct ProcInfo {
	std::string decl;
	bool anyThread = false;
	std::string access = "control";
};

/* What ptz_get_api_info says of the source's procs, by name: nothing if it
 * hasn't one */
std::map<std::string, ProcInfo> procsOf(obs_source_t *source)
{
	std::map<std::string, ProcInfo> procs;
	calldata_t cd = {};
	if (proc_handler_call(obs_source_get_proc_handler(source), "ptz_get_api_info", &cd)) {
		OBSDataAutoRelease info = static_cast<obs_data_t *>(calldata_ptr(&cd, "return"));
		OBSDataArrayAutoRelease list = obs_data_get_array(info, "procs");
		for (size_t i = 0; i < obs_data_array_count(list); i++) {
			OBSDataAutoRelease item = obs_data_array_item(list, i);
			ProcInfo proc;
			proc.decl = obs_data_get_string(item, "decl");
			/* One that doesn't say is taken as the device's thread only, and as control */
			proc.anyThread = strcmp(obs_data_get_string(item, "thread"), "any") == 0;
			if (obs_data_has_user_value(item, "access"))
				proc.access = obs_data_get_string(item, "access");
			procs[obs_data_get_string(item, "name")] = proc;
		}
	}
	calldata_free(&cd);
	return procs;
}

/* A discovery instance has the same version proc as a device, and is told
 * from one by what else it has */
bool isDiscovery(const std::map<std::string, ProcInfo> &procs)
{
	return procs.count("ptz_discovery_start") > 0;
}

/* --- the requests there are ------------------------------------------------------ */

/* Registers with obs-websocket the requests the setting admits, and takes
 * away the ones it doesn't. Main thread. */
void applyAccess()
{
	if (!vendor)
		return;
	Access level = accessLevel.load();
	for (auto &entry : requests) {
		Request *request = entry.second.get();
		bool wanted = admits(level, request->access);
		if (wanted && !request->registered)
			request->registered = obs_websocket_vendor_register_request(vendor, request->name.c_str(),
										    request->callback, request);
		else if (!wanted && request->registered)
			request->registered = !obs_websocket_vendor_unregister_request(vendor, request->name.c_str());
	}
}

/* Has a request by this name, if there isn't one. Main thread. */
void addRequest(const std::string &name, const std::string &access, obs_websocket_request_callback_function callback)
{
	if (requests.count(name))
		return;
	auto request = std::make_unique<Request>();
	request->name = name;
	request->access = access;
	request->callback = callback;
	requests[name] = std::move(request);
}

/* --- a proc, called for a client ------------------------------------------------- */

/* The JSON fields of a request as the calldata of the proc declared `decl`.
 * False, with the error in `response`, if they aren't what it takes. A ptr is
 * an obs_data_t, kept in `held` until the proc has been called. */
bool fillCalldata(const ptzws::Decl &decl, obs_data_t *request, calldata_t *cd, std::vector<OBSData> &held,
		  obs_data_t *response)
{
	for (obs_data_item_t *item = obs_data_first(request); item; obs_data_item_next(&item)) {
		std::string key = obs_data_item_get_name(item);
		if (key == "device")
			continue;
		const ptzws::Param *param = nullptr;
		for (const ptzws::Param &p : decl.params)
			if (p.name == key)
				param = &p;
		if (!param) {
			setError(response, "unknown_field", decl.name + " has no field '" + key + "'");
			obs_data_item_release(&item);
			return false;
		}
		/* What the proc sets is not the caller's to give */
		if (param->out)
			continue;
		enum obs_data_type type = obs_data_item_gettype(item);
		bool ok = false;
		switch (param->type) {
		case ptzws::Type::String:
			ok = type == OBS_DATA_STRING;
			if (ok)
				calldata_set_string(cd, key.c_str(), obs_data_item_get_string(item));
			break;
		case ptzws::Type::Bool:
			ok = type == OBS_DATA_BOOLEAN;
			if (ok)
				calldata_set_bool(cd, key.c_str(), obs_data_item_get_bool(item));
			break;
		case ptzws::Type::Float:
			/* A whole number is one too: JSON has no way to say 1.0 */
			ok = type == OBS_DATA_NUMBER;
			if (ok)
				calldata_set_float(cd, key.c_str(), obs_data_item_get_double(item));
			break;
		case ptzws::Type::Int: {
			double value = type == OBS_DATA_NUMBER ? obs_data_item_get_double(item) : 0.5;
			ok = type == OBS_DATA_NUMBER && value == (double)(long long)value;
			if (ok)
				calldata_set_int(cd, key.c_str(), (long long)value);
			break;
		}
		case ptzws::Type::Ptr:
			ok = type == OBS_DATA_OBJECT;
			if (ok) {
				OBSDataAutoRelease object = obs_data_item_get_obj(item);
				held.push_back(object.Get());
				calldata_set_ptr(cd, key.c_str(), object.Get());
			}
			break;
		case ptzws::Type::Void:
			break;
		}
		if (!ok) {
			setError(response, "bad_type", decl.name + ": '" + key + "' is not of the declared type");
			obs_data_item_release(&item);
			return false;
		}
	}
	/* A ptr the caller left out is one for the proc to fill in, as ptz_get_state's is */
	for (const ptzws::Param &param : decl.params) {
		if (param.type != ptzws::Type::Ptr || param.out || obs_data_has_user_value(request, param.name.c_str()))
			continue;
		OBSDataAutoRelease object = obs_data_create();
		held.push_back(object.Get());
		calldata_set_ptr(cd, param.name.c_str(), object.Get());
	}
	return true;
}

/* One field of the calldata, after the proc has run, into the response. `own`
 * says a ptr is a new object the proc returned, which is released here. */
void readField(calldata_t *cd, const std::string &name, ptzws::Type type, bool own, obs_data_t *response)
{
	const char *key = name.c_str();
	switch (type) {
	case ptzws::Type::String: {
		const char *text = nullptr;
		if (calldata_get_string(cd, key, &text))
			obs_data_set_string(response, key, text ? text : "");
		break;
	}
	case ptzws::Type::Bool: {
		bool value = false;
		if (calldata_get_bool(cd, key, &value))
			obs_data_set_bool(response, key, value);
		break;
	}
	case ptzws::Type::Int: {
		long long value = 0;
		if (calldata_get_int(cd, key, &value))
			obs_data_set_int(response, key, value);
		break;
	}
	case ptzws::Type::Float: {
		double value = 0.0;
		if (calldata_get_float(cd, key, &value))
			obs_data_set_double(response, key, value);
		break;
	}
	case ptzws::Type::Ptr: {
		void *ptr = nullptr;
		if (calldata_get_ptr(cd, key, &ptr) && ptr) {
			auto data = static_cast<obs_data_t *>(ptr);
			obs_data_set_obj(response, key, data);
			if (own)
				obs_data_release(data);
		}
		break;
	}
	case ptzws::Type::Void:
		break;
	}
}

/* A proc being called for a request: the device, which is held until the proc
 * has been called and what it returned read (which is what keeps its
 * proc_handler good), and what it is called with. It is the request's until
 * the proc is sent to the main thread, and from then whichever's lets go last. */
struct ProcCall {
	OBSSource source;
	std::string name;
	calldata_t cd = {};
	std::vector<OBSData> held;
	bool called = false;

	~ProcCall() { calldata_free(&cd); }
};

/* A request for a proc of the API: obs-websocket calls it, on a thread of its own */
void procRequest(obs_data_t *requestData, obs_data_t *response, void *priv)
{
	InFlight running;
	if (closing.load()) {
		setError(response, "unavailable", "OBS is closing");
		return;
	}
	auto request = static_cast<Request *>(priv);
	std::string uuid = obs_data_get_string(requestData, "device");

	/* The camera selected in the dock is the one it is as the request comes,
	 * and the answer says which, for a client to send what follows to that
	 * one: the release of a move, say, which the selection may have left */
	if (uuid == kSelectedDevice) {
		uuid = selectedDevice();
		if (uuid.empty()) {
			setError(response, "no_selection", "no camera is selected in the PTZ dock");
			return;
		}
		obs_data_set_string(response, "device", uuid.c_str());
	}

	OBSSourceAutoRelease source = !uuid.empty() ? obs_get_source_by_uuid(uuid.c_str()) : nullptr;
	if (!source || !ptz_source_is_device(source)) {
		setError(response, "unknown_device", "no PTZ device '" + uuid + "'");
		return;
	}
	auto procs = procsOf(source);
	auto found = procs.find(request->name);
	if (found == procs.end()) {
		setError(response, "no_such_proc", "the device has no " + request->name);
		return;
	}
	const ProcInfo &proc = found->second;
	/* The setting changed after obs-websocket found the request, or this
	 * device's proc is not what the request was registered for */
	if (!admits(accessLevel.load(), proc.access)) {
		setError(response, "forbidden", request->name + " is not allowed by the PTZ websocket access setting");
		return;
	}
	auto decl = ptzws::parse_decl(proc.decl);
	if (!decl) {
		setError(response, "no_such_proc", "can't read the declaration '" + proc.decl + "'");
		return;
	}

	auto call = std::make_shared<ProcCall>();
	call->source = source.Get();
	call->name = request->name;
	if (!fillCalldata(*decl, requestData, &call->cd, call->held, response))
		return;

	auto work = [call]() {
		call->called =
			proc_handler_call(obs_source_get_proc_handler(call->source), call->name.c_str(), &call->cd);
	};
	if (proc.anyThread)
		work();
	else if (!hopDone(onMainThread(work), request->name, response))
		return;
	if (!call->called) {
		setError(response, "no_such_proc", "the device has no " + request->name);
		return;
	}

	for (const ptzws::Param &param : decl->params)
		if (param.out || param.type == ptzws::Type::Ptr)
			readField(&call->cd, param.name, param.type, false, response);
	if (decl->ret != ptzws::Type::Void)
		readField(&call->cd, decl->retName, decl->ret, true, response);
}

/* --- a signal, sent on as an event ----------------------------------------------- */

/* A signal of the API, connected to on every source that has it: what its
 * calldata has, by its declaration. Never freed, as a signal may be about to
 * be raised with it. */
struct SignalLink {
	std::string name;
	ptzws::Decl decl;
};

/* By declaration. Main thread. */
std::map<std::string, std::unique_ptr<SignalLink>> signalLinks;

/* A source whose signals are connected to */
struct Connected {
	OBSWeakSource source;
	std::vector<SignalLink *> links;
	bool discovery = false;
};

/* By UUID. A source is added on the main thread, and taken out on whichever
 * destroys it. */
std::mutex connectedMutex;
std::map<std::string, Connected> connected;

void emitEvent(const char *name, obs_data_t *data)
{
	if (vendor && !closing.load() && accessLevel.load() != Access::Off)
		obs_websocket_vendor_emit_event(vendor, name, data);
}

/* A signal of the API has been raised: on any thread */
void signalRaised(void *data, calldata_t *cd)
{
	InFlight running;
	auto link = static_cast<SignalLink *>(data);
	OBSDataAutoRelease event = obs_data_create();
	for (const ptzws::Param &param : link->decl.params) {
		const char *key = param.name.c_str();
		if (param.type == ptzws::Type::Ptr && param.name == "source") {
			/* The source that raised it, which a client knows by its UUID */
			auto source = static_cast<obs_source_t *>(calldata_ptr(cd, key));
			obs_data_set_string(event, "device", source ? obs_source_get_uuid(source) : "");
		} else {
			/* A ptr is the plugin's object, which is only read: obs-websocket
			 * has made its JSON of it by the time the event has been emitted */
			readField(cd, param.name, param.type, false, event);
		}
	}
	emitEvent(link->name.c_str(), event);
}

/* A source whose signals are connected to is being destroyed */
void sourceDestroyed(void *, calldata_t *cd)
{
	InFlight running;
	auto source = static_cast<obs_source_t *>(calldata_ptr(cd, "source"));
	if (!source)
		return;
	std::string uuid = obs_source_get_uuid(source);
	bool device = false;
	{
		std::lock_guard<std::mutex> lock(connectedMutex);
		auto found = connected.find(uuid);
		if (found == connected.end())
			return;
		device = !found->second.discovery;
		connected.erase(found);
	}
	if (!device)
		return;
	/* No signal of the API says a device has gone */
	OBSDataAutoRelease event = obs_data_create();
	obs_data_set_string(event, "device", uuid.c_str());
	emitEvent("ptz_device_removed", event);
}

/* Connects to the signals the source says it has, if it hasn't been. Main thread. */
void connectSignals(obs_source_t *source, bool discovery)
{
	std::string uuid = obs_source_get_uuid(source);
	{
		std::lock_guard<std::mutex> lock(connectedMutex);
		if (connected.count(uuid))
			return;
	}
	Connected entry;
	entry.source = OBSGetWeakRef(source);
	entry.discovery = discovery;
	signal_handler_t *handler = obs_source_get_signal_handler(source);

	calldata_t cd = {};
	if (proc_handler_call(obs_source_get_proc_handler(source), "ptz_get_api_info", &cd)) {
		OBSDataAutoRelease info = static_cast<obs_data_t *>(calldata_ptr(&cd, "return"));
		OBSDataArrayAutoRelease list = obs_data_get_array(info, "signals");
		for (size_t i = 0; i < obs_data_array_count(list); i++) {
			OBSDataAutoRelease item = obs_data_array_item(list, i);
			std::string text = obs_data_get_string(item, "decl");
			if (!signalLinks.count(text)) {
				auto decl = ptzws::parse_decl(text);
				if (!decl)
					continue;
				auto link = std::make_unique<SignalLink>();
				link->name = decl->name;
				link->decl = *decl;
				signalLinks[text] = std::move(link);
			}
			SignalLink *link = signalLinks[text].get();
			signal_handler_connect(handler, link->name.c_str(), signalRaised, link);
			entry.links.push_back(link);
			exposed[discovery ? "discovery-signal" : "device-signal"].insert(link->name);
		}
	}
	calldata_free(&cd);
	signal_handler_connect(handler, "destroy", sourceDestroyed, nullptr);

	std::lock_guard<std::mutex> lock(connectedMutex);
	connected[uuid] = entry;
}

/* Lets go of every signal, of the sources that are still there. Main thread. */
void disconnectSignals()
{
	std::map<std::string, Connected> were;
	{
		std::lock_guard<std::mutex> lock(connectedMutex);
		were.swap(connected);
	}
	for (auto &entry : were) {
		OBSSourceAutoRelease source = OBSGetStrongRef(entry.second.source);
		if (!source)
			continue;
		signal_handler_t *handler = obs_source_get_signal_handler(source);
		for (SignalLink *link : entry.second.links)
			signal_handler_disconnect(handler, link->name.c_str(), signalRaised, link);
		signal_handler_disconnect(handler, "destroy", sourceDestroyed, nullptr);
	}
}

/* --- finding devices ------------------------------------------------------------ */

/* A reference to every source there is, a filter or not, private or not */
std::vector<OBSSource> allSources()
{
	std::vector<OBSSource> sources;
	/* Only referenced under OBS's lock on its sources: a proc is not called until it is let go */
	obs_enum_all_sources(
		[](void *data, obs_source_t *source) {
			static_cast<std::vector<OBSSource> *>(data)->push_back(source);
			return true;
		},
		&sources);
	return sources;
}

/* Has a request for each proc the source says it has, and hears its signals,
 * if it is a device or a discovery instance. Main thread. */
void considerSource(obs_source_t *source)
{
	if (!source || !ptz_source_is_device(source) || ptz_device_on_private_source(source))
		return;
	auto procs = procsOf(source);
	bool discovery = isDiscovery(procs);
	for (const auto &proc : procs) {
		if (proc.second.access != "read" && proc.second.access != "control")
			continue;
		addRequest(proc.first, proc.second.access, procRequest);
		exposed[discovery ? "discovery-proc" : "device-proc"].insert(proc.first);
	}
	applyAccess();
	connectSignals(source, discovery);
}

/* OBS's own signals that a source, or a filter on one, has been made: on
 * whatever thread made it */
void sourceCreated(void *, calldata_t *cd)
{
	auto source = static_cast<obs_source_t *>(calldata_ptr(cd, "source"));
	OBSWeakSource weak = OBSGetWeakRef(source);
	QMetaObject::invokeMethod(
		qApp,
		[weak]() {
			OBSSourceAutoRelease strong = OBSGetStrongRef(weak);
			considerSource(strong);
		},
		Qt::QueuedConnection);
}

void filterAdded(void *, calldata_t *cd)
{
	auto filter = static_cast<obs_source_t *>(calldata_ptr(cd, "filter"));
	OBSWeakSource weak = OBSGetWeakRef(filter);
	QMetaObject::invokeMethod(
		qApp,
		[weak]() {
			OBSSourceAutoRelease strong = OBSGetStrongRef(weak);
			considerSource(strong);
		},
		Qt::QueuedConnection);
}

/* --- the binding's own requests ---------------------------------------------------- */

void bridgeVersion(obs_data_t *, obs_data_t *response, void *)
{
	InFlight running;
	obs_data_set_int(response, "major", kBridgeVersionMajor);
	obs_data_set_int(response, "minor", kBridgeVersionMinor);
}

/* Every device there is, with what a client needs to call it */
void listDevices(obs_data_t *, obs_data_t *response, void *)
{
	InFlight running;
	if (closing.load()) {
		setError(response, "unavailable", "OBS is closing");
		return;
	}
	OBSDataArrayAutoRelease devices = obs_data_array_create();
	/* The devices the binding has not come to know of yet: OBS has made them,
	 * and what was queued for the main thread when it did has not run */
	auto unknown = std::make_shared<std::vector<OBSSource>>();
	for (const OBSSource &source : allSources()) {
		if (!ptz_source_is_device(source) || isDiscovery(procsOf(source)) ||
		    ptz_device_on_private_source(source))
			continue;
		{
			std::lock_guard<std::mutex> lock(connectedMutex);
			if (!connected.count(obs_source_get_uuid(source)))
				unknown->push_back(source);
		}
		proc_handler_t *handler = obs_source_get_proc_handler(source);
		OBSDataAutoRelease device = obs_data_create();
		obs_data_set_string(device, "device", obs_source_get_uuid(source));
		obs_data_set_string(device, "name", obs_source_get_name(source));

		/* The source whose video the camera shows: the device says, or if
		 * it hasn't the proc, the one a filter is on, or the source itself */
		calldata_t cd = {};
		OBSSourceAutoRelease parent;
		if (proc_handler_call(handler, "ptz_get_parent_source", &cd))
			parent = static_cast<obs_source_t *>(calldata_ptr(&cd, "return"));
		else if (obs_source_get_type(source) == OBS_SOURCE_TYPE_FILTER)
			parent = obs_source_get_ref(obs_filter_get_parent(source));
		else
			parent = obs_source_get_ref(source);
		calldata_free(&cd);
		obs_data_set_string(device, "source", parent ? obs_source_get_name(parent) : "");

		cd = {};
		OBSDataAutoRelease api = obs_data_create();
		if (proc_handler_call(handler, "ptz_get_api_version", &cd)) {
			obs_data_set_int(api, "major", calldata_int(&cd, "major"));
			obs_data_set_int(api, "minor", calldata_int(&cd, "minor"));
		}
		calldata_free(&cd);
		obs_data_set_obj(device, "api", api);
		obs_data_array_push_back(devices, device);
	}
	/* A client is not told of a device before it can call it: the requests
	 * for one that is new are made now, on the main thread, and not when the
	 * main thread gets to it. Only then is the main thread waited for, and
	 * if it can't be, the list is still the list. */
	if (!unknown->empty())
		onMainThread([unknown]() {
			for (const OBSSource &source : *unknown)
				considerSource(source);
		});
	obs_data_set_array(response, "devices", devices);
}

/* The camera selected in the dock */
void getSelected(obs_data_t *, obs_data_t *response, void *)
{
	InFlight running;
	obs_data_set_string(response, "device", selectedDevice().c_str());
}

/* The discovery instances a client can use, one of each kind there is, which
 * are made the first time they are asked for and kept until the plugin
 * unloads: OBS has none of its own but while the dialog that adds a device is
 * open, and a client has no way to make one. */
void listDiscovery(obs_data_t *, obs_data_t *response, void *)
{
	InFlight running;
	/* A source is made on the main thread, which fills in a list of its own */
	OBSDataArrayAutoRelease made = obs_data_array_create();
	OBSDataArray instances = made.Get();
	auto work = [instances]() {
		for (const PTZDriverKind &kind : ptz_discovery_kinds()) {
			std::string id = kind.kind.toStdString();
			if (!discoveryInstances.count(id)) {
				OBSSourceAutoRelease instance =
					obs_source_create_private(id.c_str(), "ptz-websocket", nullptr);
				if (!instance)
					continue;
				discoveryInstances[id] = instance.Get();
				considerSource(instance);
			}
			OBSDataAutoRelease item = obs_data_create();
			obs_data_set_string(item, "device", obs_source_get_uuid(discoveryInstances[id]));
			obs_data_set_string(item, "kind", id.c_str());
			obs_data_array_push_back(instances, item);
		}
	};
	if (hopDone(onMainThread(work), "ptz_list_discovery", response))
		obs_data_set_array(response, "instances", instances);
}

/* What there is a request or an event for, of the API's procs and signals */
void listApi(obs_data_t *, obs_data_t *response, void *)
{
	InFlight running;
	OBSDataAutoRelease made = obs_data_create();
	OBSData api = made.Get();
	auto work = [api]() {
		for (const char *scope : {"device-proc", "device-signal", "discovery-proc", "discovery-signal"}) {
			OBSDataArrayAutoRelease list = obs_data_array_create();
			for (const std::string &name : exposed[scope]) {
				OBSDataAutoRelease item = obs_data_create();
				obs_data_set_string(item, "name", name.c_str());
				obs_data_array_push_back(list, item);
			}
			obs_data_set_array(api, scope, list);
		}
	};
	if (hopDone(onMainThread(work), "ptz_list_api", response))
		obs_data_apply(response, api);
}

/* Stops anything more being started, and has whatever is waiting for the main
 * thread give up. Main thread. */
void beginClosing()
{
	{
		std::lock_guard<std::mutex> lock(hopMutex);
		closing.store(true);
	}
	hopChanged.notify_all();
}

/* Gives back everything obs-websocket gave the binding, while obs-websocket is
 * still there: OBS unloads its modules in the reverse of the order it loaded
 * them, which for two in one directory is by name, so obs-websocket goes before
 * obs-ptz, and frees the vendor and the proc handler it is reached by. Nothing
 * is asked of it after this. Main thread. */
void letGoOfObsWebsocket()
{
	if (!vendor)
		return;
	accessLevel.store(Access::Off);
	applyAccess();
	vendor = nullptr;
	blog(LOG_INFO, "[obs-ptz] websocket: let go of obs-websocket");
}

/* OBS is closing. It has stopped running what the main thread is sent well
 * before it unloads a plugin, and obs-websocket, which waits for its requests
 * to finish, is unloaded first: so it is here that waiting stops, and here that
 * the binding lets go of obs-websocket, and of the sources it made. */
void frontendEvent(enum obs_frontend_event event, void *)
{
	if (event != OBS_FRONTEND_EVENT_EXIT && event != OBS_FRONTEND_EVENT_SCRIPTING_SHUTDOWN)
		return;
	beginClosing();
	letGoOfObsWebsocket();
	disconnectSignals();
	discoveryInstances.clear();
}

Access accessNamed(const char *access)
{
	if (strcmp(access, "off") == 0)
		return Access::Off;
	if (strcmp(access, "read-only") == 0)
		return Access::ReadOnly;
	if (strcmp(access, "control") == 0)
		return Access::Control;
	/* What isn't one of the three was meant to be one of them, and not
	 * necessarily the one that lets a client do everything */
	return Access::Off;
}

} // namespace

void ptz_websocket_set_access(const char *access)
{
	accessLevel.store(accessNamed(access));
	applyAccess();
}

void ptz_websocket_set_selected(const char *uuid)
{
	{
		std::lock_guard<std::mutex> lock(selectedMutex);
		if (selectedUuid == uuid)
			return;
		selectedUuid = uuid;
	}
	OBSDataAutoRelease event = obs_data_create();
	obs_data_set_string(event, "device", uuid);
	emitEvent("ptz_selected_changed", event);
}

void ptz_websocket_load(void)
{
	vendor = obs_websocket_register_vendor("obs-ptz");
	if (!vendor) {
		blog(LOG_INFO,
		     "[obs-ptz] no obs-websocket, or its 'obs-ptz' vendor is taken: the PTZ API is not on it");
		return;
	}
	if (PTZControls::getInstance())
		accessLevel.store(accessNamed(QT_TO_UTF8(PTZControls::getInstance()->websocketAccess())));

	addRequest("ptz_bridge_version", "read", bridgeVersion);
	addRequest("ptz_list_devices", "read", listDevices);
	addRequest("ptz_get_selected", "read", getSelected);
	/* It makes sources, which only a client that can use them is let do */
	addRequest("ptz_list_discovery", "control", listDiscovery);
	addRequest("ptz_list_api", "read", listApi);

	/* No source has been loaded yet, but one that a module made is there */
	signal_handler_t *global = obs_get_signal_handler();
	signal_handler_connect(global, "source_create", sourceCreated, nullptr);
	signal_handler_connect(global, "source_filter_add", filterAdded, nullptr);
	for (const OBSSource &source : allSources())
		considerSource(source);
	applyAccess();
	obs_frontend_add_event_callback(frontendEvent, nullptr);
	loaded = true;
}

void ptz_websocket_unload(void)
{
	if (!loaded)
		return;
	loaded = false;
	signal_handler_t *global = obs_get_signal_handler();
	signal_handler_disconnect(global, "source_create", sourceCreated, nullptr);
	signal_handler_disconnect(global, "source_filter_add", filterAdded, nullptr);
	obs_frontend_remove_event_callback(frontendEvent, nullptr);
	/* obs-websocket may have been unloaded by now, and is not to be asked
	 * anything: it was let go of when OBS said it was closing. If OBS never
	 * said so, its requests stay registered, and are answered "unavailable"
	 * for as long as this code is there to answer. */
	beginClosing();
	disconnectSignals();
	/* The requests and signals that have started are let leave this code
	 * before it goes. None of them waits for this thread any more. */
	auto deadline = std::chrono::steady_clock::now() + kUnloadWait;
	while (inFlight.load() > 0 && std::chrono::steady_clock::now() < deadline)
		std::this_thread::sleep_for(std::chrono::milliseconds(5));
	if (inFlight.load() > 0)
		blog(LOG_WARNING, "[obs-ptz] %d websocket requests were still running at unload", inFlight.load());
	discoveryInstances.clear();
}
