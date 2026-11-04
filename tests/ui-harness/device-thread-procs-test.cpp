/* PTZ UI test harness: a device's procs, called from a thread of the caller's own
 *
 * Copyright 2026 Grant Likely <grant.likely@secretlab.ca>
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include "ui-test-harness.hpp"

#include <obs.hpp>
#include <obs-module.h>

#include <chrono>
#include <condition_variable>
#include <deque>
#include <mutex>
#include <string>
#include <thread>

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>

namespace {

/* The preset signals the device raises, as they are heard, which is on the
 * device's own thread: a thread that waits for one is woken */
struct Heard {
	std::mutex lock;
	std::condition_variable changed;
	std::deque<std::pair<std::string, std::string>> events;
};

struct Listener {
	Heard *heard;
	const char *name;
};

void onSignal(void *data, calldata_t *cd)
{
	auto listener = static_cast<Listener *>(data);
	const char *id = "";
	if (!calldata_get_string(cd, "id", &id) || !id)
		id = "";
	const char *request = nullptr;
	std::string what = id;
	if (calldata_get_string(cd, "request", &request) && request)
		what = std::string(request) + "=" + id;
	/* A rename, told apart from a thumbnail captured meanwhile */
	auto changed = static_cast<obs_data_t *>(calldata_ptr(cd, "changed"));
	if (changed && obs_data_has_user_value(changed, "name"))
		what += "+name";
	std::lock_guard<std::mutex> locker(listener->heard->lock);
	listener->heard->events.emplace_back(listener->name, what);
	listener->heard->changed.notify_all();
}

const char *const kSignals[] = {"ptz_preset_added",         "ptz_preset_removed",    "ptz_preset_changed",
				"ptz_preset_order_changed", "ptz_preset_list_reset", "ptz_preset_create_done"};

class Run {
public:
	explicit Run(obs_source_t *filter) : m_filter(filter)
	{
		for (const char *name : kSignals)
			m_listeners.push_back({&m_heard, name});
		signal_handler_t *sh = obs_source_get_signal_handler(m_filter);
		for (Listener &listener : m_listeners)
			signal_handler_connect(sh, listener.name, onSignal, &listener);
	}
	~Run()
	{
		signal_handler_t *sh = obs_source_get_signal_handler(m_filter);
		for (Listener &listener : m_listeners)
			signal_handler_disconnect(sh, listener.name, onSignal, &listener);
	}

	/* Waits for `name` about `what` (an id, or "request=id" for create_done,
	 * where "request=" matches any id), taking it and every signal heard before it */
	bool waitFor(const std::string &name, const std::string &what, std::string *full = nullptr)
	{
		std::unique_lock<std::mutex> locker(m_heard.lock);
		auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
		for (;;) {
			while (!m_heard.events.empty()) {
				auto [heardName, heardWhat] = m_heard.events.front();
				m_heard.events.pop_front();
				m_order.append(QString::fromStdString(heardName + " " + heardWhat));
				if (heardName == name && heardWhat.compare(0, what.size(), what) == 0) {
					if (full)
						*full = heardWhat;
					return true;
				}
			}
			if (m_heard.changed.wait_until(locker, deadline) == std::cv_status::timeout &&
			    m_heard.events.empty())
				return false;
		}
	}

	/* Off the device's thread: the copy made before the signals heard */
	QJsonObject list()
	{
		calldata_t cd = {};
		QJsonObject result;
		if (proc_handler_call(obs_source_get_proc_handler(m_filter), "ptz_preset_get_list", &cd)) {
			auto info = static_cast<obs_data_t *>(calldata_ptr(&cd, "return"));
			if (info) {
				result = QJsonDocument::fromJson(obs_data_get_json(info)).object();
				obs_data_release(info);
			}
		}
		calldata_free(&cd);
		return result;
	}

	bool call(const char *name, calldata_t *cd)
	{
		return proc_handler_call(obs_source_get_proc_handler(m_filter), name, cd);
	}

	QJsonArray order() const { return m_order; }

private:
	obs_source_t *m_filter;
	Heard m_heard;
	std::deque<Listener> m_listeners;
	QJsonArray m_order;
};

QString presetName(const QJsonObject &list, const QString &id)
{
	return list.value("presets").toObject().value(id).toObject().value("name").toString();
}

QString firstInOrder(const QJsonObject &list)
{
	return list.value("order").toArray().at(0).toObject().value("id").toString();
}

/* On a thread of its own: makes two presets, renames one, moves the other first,
 * moves one and deletes it at once, and reads the list after each signal. The
 * other is deleted last, unless it is to be kept. */
void procsFromThread(OBSSource filter, bool keep)
{
	QJsonObject out;
	Run run(filter);

	auto create = [&](const char *name, const char *store) {
		calldata_t cd = {};
		calldata_set_string(&cd, "name", name);
		calldata_set_string(&cd, "store", store);
		const char *request = nullptr;
		QString result;
		if (run.call("ptz_preset_create", &cd) && calldata_get_string(&cd, "return", &request) && request)
			result = QString::fromUtf8(request);
		calldata_free(&cd);
		return result;
	};

	/* Refused at once, with no device thread to ask */
	out["refused"] = create("thread-none", "no-such-store");

	QString ids[2];
	const char *names[2] = {"thread-a", "thread-b"};
	for (int i = 0; i < 2; i++) {
		QString request = create(names[i], "local");
		out[QString("request_%1").arg(i)] = request;
		std::string done;
		if (request.isEmpty() || !run.waitFor("ptz_preset_create_done", request.toStdString() + "=", &done))
			break;
		ids[i] = QString::fromStdString(done.substr(request.size() + 1));
		/* create_done comes after added: so the list has it */
		out[QString("listed_%1").arg(i)] = run.list().value("presets").toObject().contains(ids[i]);
	}
	out["id_a"] = ids[0];
	out["id_b"] = ids[1];

	/* What the caller changes once the proc has returned is not what it asked */
	{
		OBSDataAutoRelease changes = obs_data_create();
		obs_data_set_string(changes, "name", "thread-a renamed");
		calldata_t cd = {};
		calldata_set_string(&cd, "id", qUtf8Printable(ids[0]));
		calldata_set_ptr(&cd, "changes", changes.Get());
		run.call("ptz_preset_update", &cd);
		calldata_free(&cd);
		obs_data_set_string(changes, "name", "changed after");
	}
	if (run.waitFor("ptz_preset_changed", ids[0].toStdString() + "+name"))
		out["renamed"] = presetName(run.list(), ids[0]);

	{
		calldata_t cd = {};
		calldata_set_string(&cd, "id", qUtf8Printable(ids[1]));
		calldata_set_int(&cd, "index", 0);
		run.call("ptz_preset_move", &cd);
		calldata_free(&cd);
	}
	if (run.waitFor("ptz_preset_order_changed", ""))
		out["first"] = firstInOrder(run.list());

	{
		calldata_t cd = {};
		calldata_set_string(&cd, "id", qUtf8Printable(ids[0]));
		const char *mime = nullptr;
		out["thumbnail_answered"] = run.call("ptz_preset_get_thumbnail", &cd) &&
					    calldata_get_string(&cd, "mime", &mime) && mime;
		calldata_free(&cd);
	}

	/* Each after the other, as one caller asked: the move is not overtaken */
	{
		calldata_t cd = {};
		calldata_set_string(&cd, "id", qUtf8Printable(ids[1]));
		calldata_set_int(&cd, "index", 1);
		run.call("ptz_preset_move", &cd);
		calldata_free(&cd);
		calldata_t del = {};
		calldata_set_string(&del, "id", qUtf8Printable(ids[1]));
		run.call("ptz_preset_delete", &del);
		calldata_free(&del);
	}
	if (run.waitFor("ptz_preset_removed", ids[1].toStdString()))
		out["removed_listed"] = run.list().value("presets").toObject().contains(ids[1]);

	{
		calldata_t cd = {};
		run.call("ptz_preset_refresh", &cd);
		calldata_free(&cd);
		OBSDataAutoRelease requested = obs_data_create();
		obs_data_set_bool(requested, "focus_af_enabled", true);
		calldata_t state = {};
		calldata_set_ptr(&state, "state", requested.Get());
		run.call("ptz_request_state", &state);
		calldata_free(&state);
	}

	/* The last, so that the list is as the test found it, unless it is to be seen */
	if (!keep) {
		calldata_t cd = {};
		calldata_set_string(&cd, "id", qUtf8Printable(ids[0]));
		run.call("ptz_preset_delete", &cd);
		calldata_free(&cd);
		run.waitFor("ptz_preset_removed", ids[0].toStdString());
	}

	out["signals"] = run.order();
	blog(LOG_INFO, "[ptz-ui-test] device_procs_from_thread: %s",
	     QJsonDocument(out).toJson(QJsonDocument::Compact).constData());
}

/* On obs-websocket's thread, in the request: starts a thread that calls the
 * procs of the filter named "filter" on the source named "source", and logs
 * what it found as "device_procs_from_thread: <json>" */
void runProcsFromThreadTest(const QMap<QString, QString> &params)
{
	OBSSourceAutoRelease parent = obs_get_source_by_name(qUtf8Printable(params.value(QStringLiteral("source"))));
	OBSSourceAutoRelease filter =
		parent ? obs_source_get_filter_by_name(parent, qUtf8Printable(params.value(QStringLiteral("filter"))))
		       : nullptr;
	if (!filter) {
		blog(LOG_INFO, "[ptz-ui-test] device_procs_from_thread: no such filter");
		return;
	}
	bool keep = params.value(QStringLiteral("keep")) == QStringLiteral("1");
	std::thread(procsFromThread, OBSSource(filter.Get()), keep).detach();
}

} // namespace

void registerDeviceThreadProcsTest(PTZUITestHarness *harness)
{
	/* device_procs_from_thread request params:
	 *   source - the source the device's filter is on
	 *   filter - the filter's name
	 *   keep   - "1" to leave the first preset made, renamed, for the dock to show */
	harness->registerInlineTest(QStringLiteral("device_procs_from_thread"), &runProcsFromThreadTest);
}
