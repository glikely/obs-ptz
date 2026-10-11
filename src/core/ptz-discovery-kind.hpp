/* Pan Tilt Zoom Controls - a driver's device discovery as a hidden source kind
 *
 * Copyright 2026 Grant Likely <grant.likely@secretlab.ca>
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#pragma once

#include <memory>
#include <QByteArray>
#include <QCoreApplication>
#include <QMetaObject>
#include <QMutex>
#include <QObject>
#include <QString>
#include <QThread>
#include <obs.hpp>
#include "ptz.h"

/**
 * Says what a discovery instance's driver found, on the instance's own
 * signal handler (see "Discovery kinds" in docs/ptz-device-api.md). Silent
 * once the instance is being destroyed, so a driver object that outlives its
 * instance until its deleteLater() runs says nothing more. A listener may
 * call the instance's procs from inside a signal.
 */
class PTZDiscoveryReporter {
public:
	PTZDiscoveryReporter(obs_source_t *source, const char *kind)
		: weak(obs_source_get_weak_source(source)),
		  kind(kind)
	{
	}
	~PTZDiscoveryReporter() { obs_weak_source_release(weak); }
	PTZDiscoveryReporter(const PTZDiscoveryReporter &) = delete;
	PTZDiscoveryReporter &operator=(const PTZDiscoveryReporter &) = delete;
	/* ptz_device_found: `settings` holds "type" and what it takes to reach
	 * the device, nothing else */
	void found(const QString &id, const QString &name, const QString &address, obs_data_t *settings);
	/* ptz_discovery_finished */
	void finished();
	/* From the instance's destroy: says nothing after this returns */
	void detach();

private:
	obs_source_t *sourceRef();

	QMutex mutex;
	/* Weak, as the instance owns this: it keeps OBS's control block, which
	 * a strong reference is taken through, until detach(), however soon
	 * after the last strong reference goes that runs */
	obs_weak_source_t *weak;
	QByteArray kind;
};

/* Registers a discovery instance's procs and signals, in the scopes
 * "discovery-proc" and "discovery-signal" (see ptz_proc_add()) */
void ptz_discovery_add_api(obs_source_t *source, proc_handler_proc_t start, proc_handler_proc_t stop, void *data);

namespace ptz_discovery_detail {
struct KindInfo {
	const char *filterKind;
	const char *name;
};

template<class T> struct Instance {
	std::shared_ptr<PTZDiscoveryReporter> reporter;
	T *impl = nullptr;
};

/* On the driver object's own thread */
template<class T> void call(void *data, void (T::*method)())
{
	T *impl = static_cast<Instance<T> *>(data)->impl;
	if (impl)
		QMetaObject::invokeMethod(impl, [impl, method]() { (impl->*method)(); });
}
} // namespace ptz_discovery_detail

/**
 * Registers `id`, an input kind OBS never lists (OBS_SOURCE_CAP_DISABLED),
 * whose defaults carry the PTZ API marker, as the discovery of devices of
 * the filter kind `filterKind`. Each instance owns a T, made on the main
 * thread with the instance's reporter, which start() looks for devices with
 * and stop() gives up; destroying the instance deletes it. T is a QObject.
 * The strings must last as long as the module.
 */
template<class T> void ptz_discovery_register_kind(const char *id, const char *filterKind, const char *name)
{
	using namespace ptz_discovery_detail;
	struct obs_source_info info = {};
	info.id = id;
	info.type = OBS_SOURCE_TYPE_INPUT;
	info.output_flags = OBS_SOURCE_CAP_DISABLED;
	info.type_data = new KindInfo{filterKind, name};
	info.free_type_data = [](void *type_data) {
		delete static_cast<KindInfo *>(type_data);
	};
	info.get_name = [](void *type_data) -> const char * {
		return static_cast<KindInfo *>(type_data)->name;
	};
	info.get_defaults = [](obs_data_t *defaults) {
		ptz_filter_defaults_mark(defaults);
	};
	info.create = [](obs_data_t *, obs_source_t *source) -> void * {
		auto kind = static_cast<KindInfo *>(obs_source_get_type_data(source));
		auto instance = new Instance<T>;
		instance->reporter = std::make_shared<PTZDiscoveryReporter>(source, kind->filterKind);
		auto build = [instance]() {
			instance->impl = new T(instance->reporter);
		};
		/* Its sockets and timers belong on the main thread */
		if (QThread::currentThread() != qApp->thread())
			QMetaObject::invokeMethod(qApp, build, Qt::BlockingQueuedConnection);
		else
			build();
		ptz_discovery_add_api(
			source, [](void *data, calldata_t *) { call<T>(data, &T::start); },
			[](void *data, calldata_t *) { call<T>(data, &T::stop); }, instance);
		return instance;
	};
	/* OBS destroys a source on a thread of its own: stop it talking now,
	 * and delete its driver object on the driver's thread */
	info.destroy = [](void *data) {
		auto instance = static_cast<Instance<T> *>(data);
		instance->reporter->detach();
		if (instance->impl)
			instance->impl->deleteLater();
		delete instance;
	};
	obs_register_source(&info);
}
