/* Delivers "OBS has finished loading" and "OBS is closing" to the devices of
 * one module, including a device made after the first of them.
 *
 * Copyright 2026 Grant Likely <grant.likely@secretlab.ca>
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#pragma once

#include <algorithm>
#include <vector>

/* Thread model: single-threaded. Every call is made on the Qt main thread,
 * where OBS sends frontend events and where a device is constructed (see
 * ptz_filter_create()) and destroyed (ptz_filter_destroy() deleteLater()s it).
 * No locking. A Listener must not add() or remove() from its own callback.
 *
 * Each module owns one of these, next to its one frontend callback, so the
 * controller never has to reach into a device to start it. */
class PTZLifecycle {
public:
	struct Listener {
		virtual void onStartup() = 0;
		virtual void onShutdown() = 0;
		virtual ~Listener() = default;
	};

	/* A listener added once loading has finished is started at once, so one
	 * made while FINISHED_LOADING is being dispatched hears of it exactly
	 * once whichever side of loadingFinished() it was added on. */
	void add(Listener *l)
	{
		if (!l || find(l) != m_entries.end())
			return;
		m_entries.push_back({l, false});
		if (m_finished && !m_exited) {
			m_entries.back().started = true;
			l->onStartup();
		}
	}

	void remove(Listener *l)
	{
		auto it = find(l);
		if (it != m_entries.end())
			m_entries.erase(it);
	}

	/* Each current listener's onStartup() once; repeats do nothing */
	void loadingFinished()
	{
		if (m_finished)
			return;
		m_finished = true;
		for (Listener *l : snapshot()) {
			auto it = find(l);
			if (it == m_entries.end() || it->started)
				continue;
			it->started = true;
			l->onStartup();
		}
	}

	/* Each current listener's onShutdown() once; repeats do nothing */
	void exiting()
	{
		if (m_exited)
			return;
		m_exited = true;
		for (Listener *l : snapshot())
			if (find(l) != m_entries.end())
				l->onShutdown();
	}

private:
	struct Entry {
		Listener *listener;
		bool started;
	};
	std::vector<Entry>::iterator find(Listener *l)
	{
		return std::find_if(m_entries.begin(), m_entries.end(),
				    [l](const Entry &e) { return e.listener == l; });
	}
	std::vector<Listener *> snapshot() const
	{
		std::vector<Listener *> out;
		for (const Entry &e : m_entries)
			out.push_back(e.listener);
		return out;
	}

	std::vector<Entry> m_entries;
	bool m_finished = false;
	bool m_exited = false;
};
