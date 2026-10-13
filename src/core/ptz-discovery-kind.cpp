/* Pan Tilt Zoom Controls - a driver's device discovery as a hidden source kind
 *
 * Copyright 2026 Grant Likely <grant.likely@secretlab.ca>
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include "ptz-discovery-kind.hpp"

#include <QMutexLocker>
#include "ptz-device.hpp"

/* A reference to the instance's source, or none once its last one has gone.
 * The lock is held only to take it: a listener may call a proc that makes
 * the driver say something more, on this same thread, from inside a signal.
 * While the reference is held OBS cannot start destroying the source, so
 * detach() can't run, nor its destroy return, while a signal is going out. */
obs_source_t *PTZDiscoveryReporter::sourceRef()
{
	QMutexLocker locker(&mutex);
	return weak ? obs_weak_source_get_source(weak) : nullptr;
}

void PTZDiscoveryReporter::found(const QString &id, const QString &name, const QString &address, obs_data_t *settings)
{
	OBSSourceAutoRelease ref = sourceRef();
	if (!ref)
		return;
	calldata_t cd = {};
	calldata_set_ptr(&cd, "source", ref.Get());
	calldata_set_string(&cd, "kind", kind.constData());
	calldata_set_string(&cd, "id", qUtf8Printable(id));
	calldata_set_string(&cd, "name", qUtf8Printable(name));
	calldata_set_string(&cd, "address", qUtf8Printable(address));
	calldata_set_ptr(&cd, "settings", settings);
	signal_handler_signal(obs_source_get_signal_handler(ref), "ptz_device_found", &cd);
	calldata_free(&cd);
}

void PTZDiscoveryReporter::finished()
{
	OBSSourceAutoRelease ref = sourceRef();
	if (!ref)
		return;
	calldata_t cd = {};
	calldata_set_ptr(&cd, "source", ref.Get());
	signal_handler_signal(obs_source_get_signal_handler(ref), "ptz_discovery_finished", &cd);
	calldata_free(&cd);
}

void PTZDiscoveryReporter::detach()
{
	QMutexLocker locker(&mutex);
	obs_weak_source_release(weak);
	weak = nullptr;
}

void ptz_discovery_add_api(obs_source_t *source, proc_handler_proc_t start, proc_handler_proc_t stop, void *data)
{
	proc_handler_t *ph = obs_source_get_proc_handler(source);
	signal_handler_t *sh = obs_source_get_signal_handler(source);
	/* The same version as a device's: see docs/ptz-device-api.md */
	ptz_proc_add("discovery-proc", ph, "void ptz_get_api_version(out int major, out int minor)",
		     ptz_get_api_version_proc, nullptr, PTZ_THREAD_ANY, PTZ_ACCESS_READ);
	ptz_proc_add("discovery-proc", ph, "ptr ptz_get_api_info()", ptz_get_api_info_proc,
		     const_cast<char *>("discovery"), PTZ_THREAD_ANY, PTZ_ACCESS_READ);
	ptz_proc_add("discovery-proc", ph, "void ptz_discovery_start()", start, data, PTZ_THREAD_ANY);
	ptz_proc_add("discovery-proc", ph, "void ptz_discovery_stop()", stop, data, PTZ_THREAD_ANY);
	ptz_signal_add("discovery-signal", sh,
		       "void ptz_device_found(ptr source, string kind, string id, string name, string address, "
		       "ptr settings)");
	ptz_signal_add("discovery-signal", sh, "void ptz_discovery_finished(ptr source)");
}
