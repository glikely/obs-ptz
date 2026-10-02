/* Pan Tilt Zoom Controls - PTZDevice base object
 *
 * Copyright 2020-2026 Grant Likely <grant.likely@secretlab.ca>
 *
 * SPDX-License-Identifier: GPLv2+
 */

#include <obs.hpp>
#include <algorithm>
#include <ctime>
#include <functional>
#include <QCoreApplication>
#include <QHash>
#include <QMutex>
#include <QThread>
#include <QUrl>
#include "ptz-device.hpp"
#include "ptz-list-model.hpp"
#include "ptz-discovery.hpp"
#include "ptz-thumbnail.hpp"
#include "ptz-visca-udp.hpp"
#include "ptz-visca-tcp.hpp"
#include "ptz-onvif.hpp"
#include "ptz-onvif-discovery.hpp"
#include "ptz-sony-discovery.hpp"
#include "ptz-usb-cam.hpp"
#include "ptz.h"
#include "protocol-helpers.hpp"

#if defined(ENABLE_SERIALPORT)
#include "ptz-visca-uart.hpp"
#include "ptz-pelco.hpp"
#endif

/* Lookup table of device_id to PTZDevice instances. Guarded by
 * ptz_device_registry_mutex since ptz_device_create()/ptz_device_destroy()
 * can be called from any thread */
static QRecursiveMutex ptz_device_registry_mutex;
static QHash<uint32_t, PTZDevice *> ptz_device_registry;

/**
 * Lambda factory macro for the PTZ proc_handler methods. This macro
 * simplifies the registration of PTZDevice methods as targets for
 * proc_handler calls.
 *
 * In this current implementation, the proc_handler can be called from
 * any thread, and the calldata method must decode the arguments and use
 * invokeMethod to call the real target. invokeMethod will check if it
 * was called from the object's thread. If it wasn't, and if the method
 * doesn't return anything, then the call is queued on the correct
 * thread. For methods that do return data, they aren't handled yet and
 * will log an error when calling from a different thread.
 */
#define ptz_ph_lambda(_method) [](void *p, calldata_t *cd) \
	{ \
		auto ptz = static_cast<PTZDevice *>(p); \
		if (!ptz) { \
			blog(LOG_ERROR, "PTZ proc_handler called without PTZDevice pointer"); \
			return; \
		} \
		ptz->_method(cd); \
	}

PTZDevice::PTZDevice(OBSData config, obs_source_t *filter) : QObject()
{
	if (filter)
		m_filter = OBSGetWeakRef(filter);

	/* proc_handers for calling into the device. Comes from the filter on
	 * filter-owned devices. Allocated new otherwise */
	handler = filter ? obs_source_get_proc_handler(filter) : proc_handler_create();
	if (!handler) {
		blog(LOG_ERROR, "could not allocate proc_handler for %s", obs_data_get_string(config, "name"));
		return;
	}

	auto get_api_version = [](void *, calldata_t *cd) {
		calldata_set_int(cd, "major", PTZ_API_VERSION_MAJOR);
		calldata_set_int(cd, "minor", PTZ_API_VERSION_MINOR);
	};
	/* The version of the PTZ API this device implements. A device may come
	 * from another plugin, at another version than the ptz_get_api_version
	 * on OBS's proc_handler reports, so a caller checks each device's own
	 * before relying on anything else it has. */
	proc_handler_add(handler, "void ptz_get_api_version(out int major, out int minor)", get_api_version, nullptr);

	/* The PTZ Device API. All these functions are prefixed with 'ptz_' so that they can
	 * be added to an existing proc_handler with low risk of conflicts */
	proc_handler_add(handler, "void ptz_stop()", ptz_ph_lambda(stop), this);
	proc_handler_add(handler, "void ptz_home_recall()", ptz_ph_lambda(pantilt_home), this);
	proc_handler_add(handler, "void ptz_home_save()", ptz_ph_lambda(pantilt_set_home), this);
	proc_handler_add(handler, "void ptz_move()", ptz_ph_lambda(move), this);
	proc_handler_add(handler, "void ptz_move_abs()", ptz_ph_lambda(move_abs), this);
	proc_handler_add(handler, "void ptz_move_rel()", ptz_ph_lambda(move_rel), this);
	proc_handler_add(handler, "void ptz_preset_save()", ptz_ph_lambda(preset_save), this);
	proc_handler_add(handler, "void ptz_preset_recall()", ptz_ph_lambda(preset_recall), this);
	proc_handler_add(handler, "void ptz_preset_clear()", ptz_ph_lambda(preset_clear), this);

	/* The device's whole state and what describes it (what PTZListModel
	 * shows a device row with, too), and locking it. What describes it
	 * includes "features", what the device can do: an object with the
	 * name of each it can true, from "pantilt", "zoom", "focus",
	 * "pantilt_abs", "pantilt_rel", "zoom_abs", "focus_abs", "home",
	 * "home_set", "autofocus", "focus_onetouch", "presets", "power",
	 * "wb_onepush" and "diagnostics". It can change, as a device finds out
	 * what the camera has. A device without "features" predates them. */
	proc_handler_add(handler, "ptr ptz_get_state(ptr state)", ptz_ph_lambda(get_state), this);
	proc_handler_add(handler, "void ptz_set_locked(bool locked)", ptz_ph_lambda(setLock), this);

	/* Settings, which are persisted, in the PTZ Control filter's own settings:
	 * what save() writes, applying new ones (through the filter, if there is
	 * one), and the properties that edit them */
	proc_handler_add(handler, "void ptz_get_config(ptr config)", ptz_ph_lambda(get_config), this);
	proc_handler_add(handler, "void ptz_set_config(ptr config)", ptz_ph_lambda(set_config), this);
	proc_handler_add(handler, "ptr ptz_get_properties()", ptz_ph_lambda(get_obs_properties), this);

	/* Transient state, which is never saved: a request to change some of it.
	 * ptz_get_state, above, reads all of it. Keys that start with "user_"
	 * are a user's own, for a camera given commands the plugin doesn't
	 * have, and never ones the plugin has. */
	proc_handler_add(handler, "void ptz_request_state(ptr state)", ptz_ph_lambda(request_state), this);

	/* One-shot actions on the camera, which aren't state. Names that start
	 * with "user_" are a user's own, as state keys are. */
	proc_handler_add(handler, "void ptz_trigger(string name)", ptz_ph_lambda(trigger), this);

	/* Preset list CRUD */
	proc_handler_add(handler, "ptr ptz_preset_get_list()", ptz_ph_lambda(preset_get_list), this);
	proc_handler_add(handler, "int ptz_preset_new(int row)", ptz_ph_lambda(newPreset), this);
	proc_handler_add(handler, "void ptz_preset_remove(int row)", ptz_ph_lambda(removePresetAtDisplayRow), this);
	proc_handler_add(handler, "void ptz_preset_move(int src_row, int dest_row)", ptz_ph_lambda(movePreset), this);
	proc_handler_add(handler, "void ptz_preset_set_name(int id, string name)", ptz_ph_lambda(setPresetName), this);

	/* The program or preview scene changed: re-check whether the device is live */
	proc_handler_add(handler, "void ptz_scene_changed()", ptz_ph_lambda(onSceneChanged), this);

	/* Signal handler for notifying state & settings changes. Shared with
	 * the filter for a filter-owned device, same as handler above. */
	sigs = filter ? obs_source_get_signal_handler(filter) : signal_handler_create();
	if (!sigs) {
		blog(LOG_ERROR, "could not allocate signal_handler for %s", obs_data_get_string(config, "name"));
	} else {
		/* The device's state changed. "changed" holds the values that
		 * changed; a listener may keep a reference to it, but not change it:
		 * every listener gets the same one, and the device never touches it
		 * again. */
		signal_handler_add(sigs, "void state_changed(int device_id, ptr changed)");

		/* The device's settings were applied, from anywhere */
		signal_handler_add(sigs, "void settings_changed(int device_id)");

		/* Preset modification signals */
		signal_handler_add(sigs, "void preset_inserted(int device_id, int row)");
		signal_handler_add(sigs, "void preset_removed(int device_id, int row)");
		signal_handler_add(sigs, "void preset_moved(int device_id, int src_row, int dest_row)");
		signal_handler_add(sigs, "void preset_renamed(int device_id, int id)");
		signal_handler_add(sigs, "void preset_thumbnail_changed(int device_id, int id)");
	}

	/* A filter-owned device is given its source by its filter, see setParentSource() */
	setParentSourceByName(isSelfManaged() ? obs_data_get_string(config, "name") : "");
	type = obs_data_get_string(config, "type");
	state = obs_data_create();
	obs_data_release(state);
	stateChanged = obs_data_create();
	obs_data_release(stateChanged);
	statistics = obs_data_create();
	obs_data_release(statistics);
	obs_data_set_obj(state, "statistics", statistics);
	stale_state = {"pan_pos", "tilt_pos", "zoom_pos", "focus_pos"};

	/* Assign a unique ID -- this is the one place a device's identity is
	 * decided, so it happens here rather than in whatever happens to be
	 * listening on the create signal that announceCreated() fires. Hold
	 * the lock across the search *and* the insert so two concurrent
	 * constructions can't settle on the same id. */
	QMutexLocker locker(&ptz_device_registry_mutex);
	uint32_t new_id = (uint32_t)obs_data_get_int(config, "id");
	while (ptz_device_registry.contains(new_id) || new_id == 0)
		new_id++;
	id = new_id;
	ptz_device_registry[id] = this;

	/* Hear of OBS finishing loading and closing directly, not from the UI
	 * that would otherwise have to pass it on. A device is made and
	 * destroyed on the main thread, where OBS sends these. */
	obs_frontend_add_event_callback(frontendEventCallback, this);
	m_frontendCallback = true;
}

void PTZDevice::frontendEventCallback(enum obs_frontend_event event, void *data)
{
	static_cast<PTZDevice *>(data)->onFrontendEvent(event);
}

void PTZDevice::onFrontendEvent(enum obs_frontend_event event)
{
	switch (event) {
	case OBS_FRONTEND_EVENT_FINISHED_LOADING:
		onOBSStartup();
		break;
	case OBS_FRONTEND_EVENT_SCRIPTING_SHUTDOWN:
		/* OBS is closing, and has not yet cleared its scenes, which
		 * destroys the filters that own devices: EXIT comes after that.
		 * This is the last time every device is still there. */
		onOBSShutdown();
		break;
	default:
		break;
	}
}

/**
 * Fires the "ptz_device_create" signal -- deliberately *not* done from the
 * constructor so that subclasses of PTZDevice can finish their
 * initialization before the announce is sent.
 */
void PTZDevice::announceCreated()
{
	calldata_t cd = {};
	calldata_set_int(&cd, "device_id", id);
	calldata_set_ptr(&cd, "proc_handler", handler);
	calldata_set_ptr(&cd, "signal_handler", sigs);
	calldata_set_ptr(&cd, "filter", m_filter.Get());
	signal_handler_signal(ptz_get_signal_handler(), "ptz_device_create", &cd);
	calldata_free(&cd);
}

PTZDevice::~PTZDevice()
{
	calldata_t cd = {};
	calldata_set_int(&cd, "device_id", id);
	signal_handler_signal(ptz_get_signal_handler(), "ptz_device_destroy", &cd);
	calldata_free(&cd);

	{
		QMutexLocker locker(&ptz_device_registry_mutex);
		ptz_device_registry.remove(id);
	}

	if (m_frontendCallback)
		obs_frontend_remove_event_callback(frontendEventCallback, this);

	/* Stop watching the source */
	watchParentSource(m_parentSource, false);

	/* Only destroy proc/signal handlers for self-managed devices.
	 * filter-owned devices use the filters handlers */
	if (isSelfManaged()) {
		proc_handler_destroy(handler);
		signal_handler_destroy(sigs);
	}
	handler = nullptr;
	sigs = nullptr;
}

obs_source_t *PTZDevice::parentSource() const
{
	QMutexLocker locker(&m_parentSourceMutex);

	/* Check if m_parentSource is still valid, and return it if true */
	if (m_parentSource) {
		obs_source_t *src = obs_weak_source_get_source(m_parentSource);
		if (src && !obs_source_removed(src))
			return src;
		if (src) {
			watchParentSource(m_parentSource, false);
			obs_source_release(src);
		}
		m_parentSource = OBSWeakSource(); /* parent source no longer valid; clear it */
	}

	/* m_parentSource isn't valid. Either we're not bound yet, or the source went away.
	 * Try looking it up via the source name. */
	if (m_parentSourceName.isEmpty())
		return nullptr;

	obs_source_t *src = obs_get_source_by_name(QT_TO_UTF8(m_parentSourceName));
	if (src && obs_source_removed(src)) {
		obs_source_release(src);
		src = nullptr;
	}
	if (src) {
		m_parentSource = OBSGetWeakRef(src);
		watchParentSource(m_parentSource, true);
	}
	return src;
}

/* The device's name is the name of its source, so it follows the source's
 * renames. The signal fires on whatever thread did the rename, and syncName()
 * notifies listeners, so hand it to the device's own thread. */
static void ptz_source_renamed_cb(void *data, calldata_t *)
{
	auto ptz = static_cast<PTZDevice *>(data);
	QMetaObject::invokeMethod(ptz, [ptz]() { ptz->syncName(); }, Qt::QueuedConnection);
}

/* The source is being deleted, and will take a filter that owns the device
 * with it once the last reference to it goes, which can be much later */
static void ptz_source_removed_cb(void *data, calldata_t *)
{
	static_cast<PTZDevice *>(data)->backup();
}

/* The source's settings changed, which may include the address it receives
 * from. Same threading as the rename signal. */
static void ptz_source_updated_cb(void *data, calldata_t *)
{
	auto ptz = static_cast<PTZDevice *>(data);
	QMetaObject::invokeMethod(ptz, [ptz]() { ptz->checkParentHost(); }, Qt::QueuedConnection);
}

/* Caller must hold m_parentSourceMutex, or be the destructor. Const because
 * parentSource() binds lazily. */
void PTZDevice::watchParentSource(const OBSWeakSource &weak, bool watch) const
{
	OBSSourceAutoRelease src = weak ? obs_weak_source_get_source(weak) : nullptr;
	if (!src)
		return;
	auto sh = obs_source_get_signal_handler(src);
	auto self = const_cast<PTZDevice *>(this);
	if (watch) {
		signal_handler_connect(sh, "rename", ptz_source_renamed_cb, self);
		signal_handler_connect(sh, "remove", ptz_source_removed_cb, self);
		signal_handler_connect(sh, "update", ptz_source_updated_cb, self);
		QMetaObject::invokeMethod(self, [self]() { self->checkParentHost(); }, Qt::QueuedConnection);
	} else {
		signal_handler_disconnect(sh, "rename", ptz_source_renamed_cb, self);
		signal_handler_disconnect(sh, "remove", ptz_source_removed_cb, self);
		signal_handler_disconnect(sh, "update", ptz_source_updated_cb, self);
	}
}

obs_source_t *PTZDevice::filterSource() const
{
	return m_filter ? obs_weak_source_get_source(m_filter) : nullptr;
}

void PTZDevice::setParentSource(obs_source_t *source)
{
	{
		QMutexLocker locker(&m_parentSourceMutex);
		watchParentSource(m_parentSource, false);
		m_parentSource = source ? OBSGetWeakRef(source) : OBSWeakSource();
		watchParentSource(m_parentSource, true);
	}
	/* any thread can call filter_{add,remove}, do nameSync on the device's thread */
	QMetaObject::invokeMethod(this, [this]() { syncName(); });
}

QString PTZDevice::parentSourceHost() const
{
	OBSSourceAutoRelease src = parentSource();
	if (!src)
		return QString();
	const char *id = obs_source_get_id(src);
	if (!id || strcmp(id, "ndi_source") != 0)
		return QString();
	OBSDataAutoRelease settings = obs_source_get_settings(src);
	QString url = QT_UTF8(obs_data_get_string(settings, "web_control_url"));
	if (url.isEmpty())
		return QString();
	return QUrl::fromUserInput(url).host();
}

void PTZDevice::checkParentHost()
{
	QString host = parentSourceHost();
	if (host == m_parentHost)
		return;
	m_parentHost = host;
	ptz_info("parent source host is now '%s'", QT_TO_UTF8(host));
	onParentHostChanged(host);
	/* save() reports the host in the settings, as the placeholder of a blank
	 * Host */
	announceSettingsChanged();
}

void PTZDevice::syncName()
{
	OBSSourceAutoRelease src = parentSource();
	if (!src)
		return;
	QString name;
	{
		QMutexLocker locker(&m_parentSourceMutex);
		name = QT_UTF8(obs_source_get_name(src));
		if (name == m_parentSourceName)
			return;
		m_parentSourceName = name;
	}
	obs_data_set_string(stateChanged, "name", QT_TO_UTF8(name));
	notifyStateChanged();
}

/* Assign the source by name. This just sets the name and clears the weak reference.
 * Actual lookup is lazy and happens when parentSource() is called. */
void PTZDevice::setParentSourceByName(const char *name)
{
	OBSSourceAutoRelease src = (name && *name) ? obs_get_source_by_name(name) : nullptr;
	if (src) {
		setParentSource(src);
		return;
	}
	{
		QMutexLocker locker(&m_parentSourceMutex);
		watchParentSource(m_parentSource, false);
		m_parentSourceName = name;
		m_parentSource = OBSWeakSource();
	}
	syncName();
}

QString PTZDevice::description() const
{
	return QString::fromStdString(type);
}

/**
 * Update state of the device when the frontend scene changes
 */
void PTZDevice::onSceneChanged()
{
	bool was_locked = locked, was_live = live, was_preview = preview;

	locked = false;
	live = false;
	preview = false;
	// Check if the device's source is in the active program scene
	// If it is then disable the pan/tilt/zoom controls
	OBSSourceAutoRelease source = parentSource();
	if (source) {
		auto program = obs_frontend_get_current_scene();
		locked = live = ptz_scene_is_source_active(program, source);
		obs_source_release(program);

		if (obs_frontend_preview_program_mode_active()) {
			auto previewScene = obs_frontend_get_current_preview_scene();
			preview = ptz_scene_is_source_active(previewScene, source);
			obs_source_release(previewScene);
		}
	}

	/* Notify the listeners if there was a state change */
	if (locked != was_locked || live != was_live || preview != was_preview) {
		obs_data_set_bool(stateChanged, "locked", locked);
		obs_data_set_bool(stateChanged, "live", live);
		obs_data_set_bool(stateChanged, "preview", preview);
		notifyStateChanged();
	}
}

void PTZDevice::stop()
{
	pantilt(0, 0);
	zoom(0);
	focus(0);
}

void PTZDevice::pantilt(double pan, double tilt)
{
	pan = std::clamp(pan * (pan_invert ? -1 : 1), -pantilt_speed_max, pantilt_speed_max);
	tilt = std::clamp(tilt * (tilt_invert ? -1 : 1), -pantilt_speed_max, pantilt_speed_max);
	if ((pan_speed == pan) && (tilt_speed == tilt))
		return;
	pan_speed = pan;
	tilt_speed = tilt;
	pantilt_changed = true;
	do_update();
}

void PTZDevice::zoom(double speed)
{
	speed = std::clamp(speed * (zoom_invert ? -1 : 1), -zoom_speed_max, zoom_speed_max);
	if (zoom_speed == speed)
		return;
	zoom_speed = speed;
	zoom_changed = true;
	do_update();
}

void PTZDevice::focus(double speed)
{
	speed = std::clamp(speed * (focus_invert ? -1 : 1), -focus_speed_max, focus_speed_max);
	if (focus_speed == speed)
		return;
	focus_speed = speed;
	focus_changed = true;
	do_update();
}

void PTZDevice::move(calldata_t *cd)
{
	double p = 0, t = 0, z = 0, f = 0;

	if (calldata_get_float(cd, "pan", &p) + calldata_get_float(cd, "tilt", &t))
		QMetaObject::invokeMethod(this, "pantilt", Q_ARG(double, p), Q_ARG(double, t));

	if (calldata_get_float(cd, "zoom", &z))
		QMetaObject::invokeMethod(this, "zoom", Q_ARG(double, z));

	if (calldata_get_float(cd, "focus", &f))
		QMetaObject::invokeMethod(this, "focus", Q_ARG(double, f));
}

void PTZDevice::move_abs(calldata_t *cd)
{
	double p = 0, t = 0, z = 0, f = 0;

	if (calldata_get_float(cd, "pan", &p) + calldata_get_float(cd, "tilt", &t))
		QMetaObject::invokeMethod(this, "pantilt_abs", Q_ARG(double, p), Q_ARG(double, t));

	if (calldata_get_float(cd, "zoom", &z))
		QMetaObject::invokeMethod(this, "zoom_abs", Q_ARG(double, z));

	if (calldata_get_float(cd, "focus", &f))
		QMetaObject::invokeMethod(this, "focus_abs", Q_ARG(double, f));
}

void PTZDevice::move_rel(calldata_t *cd)
{
	double p = 0, t = 0;

	if (calldata_get_float(cd, "pan", &p) + calldata_get_float(cd, "tilt", &t))
		QMetaObject::invokeMethod(this, "pantilt_rel", Q_ARG(double, p), Q_ARG(double, t));
}

/**
 * Runs the one-shot action named by "name" (see runTrigger()) on the device's
 * own thread, whichever thread the proc handler is called from.
 */
void PTZDevice::trigger(calldata_t *cd)
{
	const char *name = calldata_string(cd, "name");
	if (!name)
		return;
	QString action = QString::fromUtf8(name);
	QMetaObject::invokeMethod(this, [this, action]() {
		if (!runTrigger(action))
			ptz_debug("no such trigger: %s", QT_TO_UTF8(action));
	});
}

bool PTZDevice::runTrigger(const QString &name)
{
	if (name == "focus_onetouch") {
		focus_onetouch();
		return true;
	}
	return false;
}

void PTZDevice::saveState(OBSData out) const
{
	/* What the driver has read back from the camera... */
	obs_data_apply(out, state);
	/* ...and what the device itself knows, which wins */
	{
		QMutexLocker locker(&m_parentSourceMutex);
		obs_data_set_string(out, "name", QT_TO_UTF8(m_parentSourceName));
	}
	obs_data_set_string(out, "description", QT_TO_UTF8(description()));
	obs_data_set_string(out, "type", type.c_str());
	obs_data_set_bool(out, "connected", connected);
	obs_data_set_bool(out, "live", live);
	obs_data_set_bool(out, "preview", preview);
	obs_data_set_bool(out, "locked", locked);
	const Features has = features();
	saveFeatures(out, has);
	/* What there was before "features", which has them too */
	obs_data_set_bool(out, "supports_set_home", has.testFlag(HomeSet));
	obs_data_set_bool(out, "supports_diagnostics", has.testFlag(Diagnostics));
}

const QList<QPair<PTZDevice::Feature, const char *>> &PTZDevice::featureNames()
{
	static const QList<QPair<Feature, const char *>> names = {
		{PanTilt, "pantilt"},
		{Zoom, "zoom"},
		{Focus, "focus"},
		{PanTiltAbs, "pantilt_abs"},
		{PanTiltRel, "pantilt_rel"},
		{ZoomAbs, "zoom_abs"},
		{FocusAbs, "focus_abs"},
		{Home, "home"},
		{HomeSet, "home_set"},
		{AutoFocus, "autofocus"},
		{FocusOneTouch, "focus_onetouch"},
		{Presets, "presets"},
		{Power, "power"},
		{WhiteBalanceOnePush, "wb_onepush"},
		{Diagnostics, "diagnostics"},
	};
	return names;
}

void PTZDevice::saveFeatures(obs_data_t *data, Features features) const
{
	OBSDataAutoRelease names = obs_data_create();
	for (const auto &[feature, name] : featureNames()) {
		if (features.testFlag(feature))
			obs_data_set_bool(names, name, true);
	}
	obs_data_set_obj(data, "features", names);
}

void PTZDevice::featuresChanged()
{
	Features now = features();
	if (now == reportedFeatures)
		return;
	reportedFeatures = now;
	saveFeatures(stateChanged, now);
	notifyStateChanged();
}

void PTZDevice::requestState(OBSData requested)
{
	if (obs_data_has_user_value(requested, "focus_af_enabled"))
		set_autofocus(obs_data_get_bool(requested, "focus_af_enabled"));
}

bool PTZDevice::setPosition(const char *axis, double value)
{
	const bool signedAxis = !strcmp(axis, "pan") || !strcmp(axis, "tilt");
	value = std::clamp(value, signedAxis ? -1.0 : 0.0, 1.0);
	if (obs_data_has_user_value(state, axis) && fabs(obs_data_get_double(state, axis) - value) < 0.0005)
		return false;
	obs_data_set_double(state, axis, value);
	obs_data_set_double(stateChanged, axis, value);
	return true;
}

/**
 * Asks for the values in the caller's "state" object, passed to
 * requestState(); only the keys present are acted on.
 */
void PTZDevice::request_state(calldata_t *cd)
{
	if (wrongThread("ptz_request_state"))
		return;
	auto requested = static_cast<obs_data_t *>(calldata_ptr(cd, "state"));
	if (requested)
		requestState(OBSData(requested));
}

void PTZDevice::preset_save(calldata_t *cd)
{
	long long id;
	if (calldata_get_int(cd, "preset_id", &id)) {
		QMetaObject::invokeMethod(this, "memory_set", Q_ARG(int, id));
		/* The proc_handler can be called from any thread, m_presets can't */
		QMetaObject::invokeMethod(this, [this, id] { capturePresetThumbnail((size_t)id); });
	}
}

void PTZDevice::preset_recall(calldata_t *cd)
{
	long long id;
	if (calldata_get_int(cd, "preset_id", &id))
		QMetaObject::invokeMethod(this, "memory_recall", Q_ARG(int, id));
}

void PTZDevice::preset_clear(calldata_t *cd)
{
	long long id;
	if (calldata_get_int(cd, "preset_id", &id)) {
		QMetaObject::invokeMethod(this, "memory_reset", Q_ARG(int, id));
		clearPresetThumbnail((size_t)id);
	}
}

/**
 * Fills the caller-owned obs_data_t with the device's whole transient state
 */
void PTZDevice::get_state(calldata_t *cd) const
{
	if (wrongThread("ptz_get_state"))
		return;
	auto state = static_cast<obs_data_t *>(calldata_ptr(cd, "state"));
	if (!state)
		return;
	saveState(state);
}

void PTZDevice::setLock(calldata_t *cd)
{
	if (wrongThread("ptz_set_locked"))
		return;
	setLock(calldata_bool(cd, "locked"));
}

/**
 * Fills the caller-owned obs_data_t passed in via the "config" calldata
 * field, mirroring save(OBSData) const.
 */
void PTZDevice::get_config(calldata_t *cd) const
{
	if (wrongThread("ptz_get_config"))
		return;
	auto config = static_cast<obs_data_t *>(calldata_ptr(cd, "config"));
	if (config)
		save(config);
}

/**
 * A filter's settings are the persisted truth, so for a filter-owned device
 * the new settings go in through obs_source_update(): libobs merges them into
 * the filter's own settings, then calls the filter's .update (which ends up
 * in applySettings()), and the Filters dialog sees the same values. Only a
 * self-managed device, which has no filter, applies them directly.
 */
void PTZDevice::set_config(calldata_t *cd)
{
	if (wrongThread("ptz_set_config"))
		return;
	auto config = static_cast<obs_data_t *>(calldata_ptr(cd, "config"));
	if (!config)
		return;
	if (isSelfManaged()) {
		applySettings(config);
		return;
	}
	OBSSourceAutoRelease filter = filterSource();
	if (!filter)
		return; /* filter is being destroyed */
	OBSDataAutoRelease settings = obs_data_create();
	obs_data_apply(settings, config);
	stripIdentity(settings);
	obs_source_update(filter, settings);
}

void PTZDevice::get_obs_properties(calldata_t *cd)
{
	if (wrongThread("ptz_get_properties"))
		return;
	calldata_set_ptr(cd, "return", get_obs_properties());
}

/**
 * Returns a caller-owned obs_data_array_t of {id, name, token} entries in
 * display order, the preset-list equivalent of get_state() -- plus
 * "max_presets" on the same calldata, since that's the configured *limit* on
 * this list (the "preset_max" setting, see save()/update()), not
 * transient device state, so it belongs with the presets functions rather
 * than in get_state()'s snapshot.
 */
void PTZDevice::preset_get_list(calldata_t *cd) const
{
	if (wrongThread("ptz_preset_get_list"))
		return;
	obs_data_array_t *list = obs_data_array_create();
	for (auto id : m_presetsDisplayOrder) {
		obs_data_t *item = obs_data_create();
		obs_data_set_int(item, "id", id);
		obs_data_set_string(item, "name", QT_TO_UTF8(presetName(id)));
		obs_data_set_string(item, "token", QT_TO_UTF8(presetToken(id)));
		obs_data_set_string(item, "thumbnail", QT_TO_UTF8(ptz_thumbnail_path(presetThumbnail(id))));
		obs_data_array_push_back(list, item);
		obs_data_release(item);
	}
	calldata_set_ptr(cd, "return", list);
	calldata_set_int(cd, "max_presets", (long long)m_maxPresets);
}

void PTZDevice::newPreset(calldata_t *cd)
{
	if (wrongThread("ptz_preset_new"))
		return;
	long long row = -1;
	calldata_get_int(cd, "row", &row);
	calldata_set_int(cd, "return", newPreset((int)row));
}

void PTZDevice::removePresetAtDisplayRow(calldata_t *cd)
{
	if (wrongThread("ptz_preset_remove"))
		return;
	removePresetAtDisplayRow((int)calldata_int(cd, "row"));
}

void PTZDevice::movePreset(calldata_t *cd)
{
	if (wrongThread("ptz_preset_move"))
		return;
	movePreset((int)calldata_int(cd, "src_row"), (int)calldata_int(cd, "dest_row"));
}

void PTZDevice::setPresetName(calldata_t *cd)
{
	if (wrongThread("ptz_preset_set_name"))
		return;
	setPresetName((size_t)calldata_int(cd, "id"), QT_UTF8(calldata_string(cd, "name")));
}

void PTZDevice::defaults(obs_data_t *config)
{
	obs_data_set_default_int(config, "preset_max", 16);
	obs_data_set_default_double(config, "pantilt_speed_max", 1.0);
	obs_data_set_default_double(config, "zoom_speed_max", 1.0);
	obs_data_set_default_double(config, "focus_speed_max", 1.0);
	obs_data_set_default_bool(config, "pan_invert", false);
	obs_data_set_default_bool(config, "tilt_invert", false);
	obs_data_set_default_bool(config, "zoom_invert", false);
	obs_data_set_default_bool(config, "focus_invert", false);
}

void PTZDevice::applySettings(OBSData settings)
{
	update(settings);
	announceSettingsChanged();
}

void PTZDevice::announceSettingsChanged()
{
	calldata_t cd = {};
	calldata_set_int(&cd, "device_id", id);
	signal_handler_signal(sigs, "settings_changed", &cd);
	calldata_free(&cd);
}

void PTZDevice::stripIdentity(obs_data_t *settings)
{
	obs_data_erase(settings, "name");
	obs_data_erase(settings, "id");
	obs_data_erase(settings, "is-self-managed");
}

void PTZDevice::update(OBSData config)
{
	/* Clamp to the same range enforced by the properties slider; a corrupt
	 * or hand-edited config must not yield an absurd preset count. */
	m_maxPresets = std::clamp<size_t>(obs_data_get_int(config, "preset_max"), 1, 128);
	/* Update the list of preset names */
	OBSDataArrayAutoRelease preset_array = obs_data_get_array(config, "presets");
	m_presets.clear();
	m_presetsDisplayOrder.clear();
	for (size_t i = 0; i < obs_data_array_count(preset_array); i++) {
		OBSDataAutoRelease item = obs_data_array_item(preset_array, i);
		auto id = obs_data_get_int(item, "id");
		if (m_presetsDisplayOrder.contains(id))
			continue;
		QVariantMap preset = OBSDataToVariantMap(item.Get());
		m_presets[id] = preset;
		sanitizePreset(id);
	}

	if (isSelfManaged())
		setParentSourceByName(obs_data_get_string(config, "name"));
	pantilt_speed_max = obs_data_get_double(config, "pantilt_speed_max");
	zoom_speed_max = obs_data_get_double(config, "zoom_speed_max");
	focus_speed_max = obs_data_get_double(config, "focus_speed_max");
	pan_invert = obs_data_get_bool(config, "pan_invert");
	tilt_invert = obs_data_get_bool(config, "tilt_invert");
	zoom_invert = obs_data_get_bool(config, "zoom_invert");
	focus_invert = obs_data_get_bool(config, "focus_invert");
}

void PTZDevice::save(OBSData config) const
{
	/* Devices are identified by their source's name; "" for no source */
	OBSSourceAutoRelease src = parentSource();
	QString name;
	{
		QMutexLocker locker(&m_parentSourceMutex);
		name = src ? QT_UTF8(obs_source_get_name(src)) : m_parentSourceName;
	}
	obs_data_set_string(config, "name", QT_TO_UTF8(name));
	obs_data_set_int(config, "id", id);
	obs_data_set_string(config, "type", type.c_str());
	obs_data_set_bool(config, "is-self-managed", isSelfManaged());
	obs_data_set_double(config, "pantilt_speed_max", pantilt_speed_max);
	obs_data_set_double(config, "zoom_speed_max", zoom_speed_max);
	obs_data_set_double(config, "focus_speed_max", focus_speed_max);
	obs_data_set_bool(config, "pan_invert", pan_invert);
	obs_data_set_bool(config, "tilt_invert", tilt_invert);
	obs_data_set_bool(config, "zoom_invert", zoom_invert);
	obs_data_set_bool(config, "focus_invert", focus_invert);
	obs_data_set_int(config, "preset_max", m_maxPresets);

	OBSDataArrayAutoRelease preset_array = obs_data_array_create();
	for (auto id : m_presetsDisplayOrder) {
		OBSDataAutoRelease data = variantMapToOBSData(m_presets[id]);
		obs_data_set_int(data, "id", id);
		obs_data_array_push_back(preset_array, data);
	}
	obs_data_set_array(config, "presets", preset_array);
}

obs_properties_t *PTZDevice::get_obs_properties()
{
	obs_properties_t *rtn_props = obs_properties_create();

	/* For self-managed instances, provide a list of sources to bind to */
	if (isSelfManaged()) {
		/* Combo box list for associated OBS source */
		auto src_cb = [](void *data, obs_source_t *src) {
			auto srcnames = static_cast<QStringList *>(data);
			if (obs_source_get_type(src) != OBS_SOURCE_TYPE_SCENE)
				srcnames->append(obs_source_get_name(src));
			return true;
		};
		auto srcs_prop = obs_properties_add_list(rtn_props, "name", obs_module_text("PTZ.Source"),
							 OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_STRING);
		obs_property_list_add_string(srcs_prop, obs_module_text("PTZ.Device.NoSource"), "");
		/* Add current source to top list */
		OBSSourceAutoRelease src = parentSource();
		if (src)
			obs_property_list_add_string(srcs_prop, obs_source_get_name(src), obs_source_get_name(src));
		/* Add all sources not assigned to a camera */
		QStringList srcnames;
		obs_enum_sources(src_cb, &srcnames);
		{
			QMutexLocker locker(&ptz_device_registry_mutex);
			for (auto ptz : ptz_device_registry)
				srcnames.removeAll(ptz->m_parentSourceName);
		}
		for (auto n : srcnames)
			obs_property_list_add_string(srcs_prop, QT_TO_UTF8(n), QT_TO_UTF8(n));
	}

	obs_properties_t *config = obs_properties_create();
	obs_properties_add_group(rtn_props, "interface", obs_module_text("PTZ.Device.Connection"), OBS_GROUP_NORMAL,
				 config);

	/* Generic camera limits and speeds properties */
	auto speed = obs_properties_create();
	obs_properties_add_group(rtn_props, "general", obs_module_text("PTZ.Device.CameraSettings"), OBS_GROUP_NORMAL,
				 speed);
	obs_properties_add_int_slider(speed, "preset_max", obs_module_text("PTZ.Device.MaxPresets"), 1, 0x80, 1);
	obs_properties_add_float_slider(speed, "pantilt_speed_max", obs_module_text("PTZ.Device.PanTiltMaxSpeed"), 0.1,
					1.0, 1.0 / 1024);
	obs_properties_add_bool(speed, "pan_invert", obs_module_text("PTZ.Device.PanInvertAxis"));
	obs_properties_add_bool(speed, "tilt_invert", obs_module_text("PTZ.Device.TiltInvertAxis"));
	obs_properties_add_float_slider(speed, "zoom_speed_max", obs_module_text("PTZ.Device.ZoomMaxSpeed"), 0.1, 1.0,
					1.0 / 1024);
	obs_properties_add_bool(speed, "zoom_invert", obs_module_text("PTZ.Device.ZoomInvertAxis"));
	obs_properties_add_float_slider(speed, "focus_speed_max", obs_module_text("PTZ.Device.FocusMaxSpeed"), 0.1, 1.0,
					1.0 / 1024);
	obs_properties_add_bool(speed, "focus_invert", obs_module_text("PTZ.Device.FocusInvertAxis"));

	return rtn_props;
}

/**
 * Driver factory, dispatching on config["type"]. This is the one place that
 * needs to name every concrete PTZDevice subclass -- PTZListModel and
 * settings.cpp just call this (or ptz_devices_set_config() below) with an
 * OBSData and never see a driver header.
 */
void ptz_device_create(obs_data_t *config)
{
	std::string type = obs_data_get_string(config, "type");
	PTZDevice *ptz = nullptr;

#if defined(ENABLE_SERIALPORT)
	if (type == "pelco" || type == "pelco-p") {
		PTZPelco::defaults(config);
		ptz = new PTZPelco(config);
	}
#endif /* ENABLE_SERIALPORT */
	if (type == "visca" || type == "visca-over-ip" || type == "visca-over-tcp") {
		PTZVisca::defaults(config);
		ptz = new PTZVisca(config);
	}
#if defined(ENABLE_ONVIF)
	if (type == "onvif") {
		PTZOnvif::defaults(config);
		ptz = new PTZOnvif(config);
	}
#endif /* ENABLE_ONVIF */
#if defined(ENABLE_USB_CAM)
	if (type == "usb-cam") {
		PTZUSBCam::defaults(config);
		ptz = new PTZUSBCam(config);
	}
#endif /* ENABLE_USB_CAM */

	/* Only announce once the full (base + derived) object is constructed
	 * -- see PTZDevice::announceCreated()'s comment. */
	if (ptz)
		ptz->announceCreated();
}

void ptz_device_destroy(uint32_t device_id)
{
	QMutexLocker locker(&ptz_device_registry_mutex);
	auto ptz = ptz_device_registry.value(device_id, nullptr);
	/* only self managed PTZDevices get deleted here */
	if (ptz && ptz->isSelfManaged()) {
		ptz->backup();
		delete ptz;
	}
}

/**
 * Rolling backup of the settings of devices that have gone away, so that
 * one lost by deleting its source (which takes its filter, and with it the
 * presets, along) can be added again. Each entry is what save() wrote, plus
 * "backup_time"; most recent first, one per source name and type. Kept in
 * its own file, loaded on first use, and guarded by ptz_backup_mutex since a
 * filter can be destroyed on any thread.
 */
#define PTZ_BACKUP_FILE "device-backups.json"
#define PTZ_BACKUP_MAX 32
static QMutex ptz_backup_mutex;
static OBSDataArray ptz_backups;

/* Caller must hold ptz_backup_mutex */
static obs_data_array_t *ptz_backups_locked()
{
	if (ptz_backups)
		return ptz_backups;
	OBSDataAutoRelease data;
	char *file = obs_module_config_path(PTZ_BACKUP_FILE);
	if (file) {
		data = obs_data_create_from_json_file_safe(file, "bak");
		bfree(file);
	}
	OBSDataArrayAutoRelease array = data ? obs_data_get_array(data, "devices") : nullptr;
	if (!array)
		array = obs_data_array_create();
	ptz_backups = array.Get();
	return ptz_backups;
}

static void ptz_backups_write_locked()
{
	char *file = obs_module_config_path(PTZ_BACKUP_FILE);
	if (!file)
		return;
	OBSDataAutoRelease data = obs_data_create();
	obs_data_set_array(data, "devices", ptz_backups);
	if (!obs_data_save_json_pretty_safe(data, file, "tmp", "bak")) {
		char *path = obs_module_config_path("");
		if (path) {
			os_mkdirs(path);
			bfree(path);
		}
		obs_data_save_json_pretty_safe(data, file, "tmp", "bak");
	}
	bfree(file);
}

void PTZDevice::backup() const
{
	OBSDataAutoRelease entry = obs_data_create();
	save(entry.Get());
	QString name = QT_UTF8(obs_data_get_string(entry, "name"));
	/* Without a source there's nothing to recognise it by */
	if (name.isEmpty())
		return;
	obs_data_erase(entry, "id");
	obs_data_erase(entry, "is-self-managed");
	obs_data_set_int(entry, "backup_time", (long long)time(nullptr));

	QMutexLocker locker(&ptz_backup_mutex);
	obs_data_array_t *backups = ptz_backups_locked();
	for (size_t i = obs_data_array_count(backups); i-- > 0;) {
		OBSDataAutoRelease item = obs_data_array_item(backups, i);
		if (name == QT_UTF8(obs_data_get_string(item, "name")) && type == obs_data_get_string(item, "type"))
			obs_data_array_erase(backups, i);
	}
	obs_data_array_insert(backups, 0, entry);
	while (obs_data_array_count(backups) > PTZ_BACKUP_MAX)
		obs_data_array_erase(backups, obs_data_array_count(backups) - 1);
	ptz_backups_write_locked();
}

obs_data_array_t *ptz_device_backups_get()
{
	QMutexLocker locker(&ptz_backup_mutex);
	obs_data_array_t *backups = ptz_backups_locked();
	obs_data_array_t *copy = obs_data_array_create();
	for (size_t i = 0; i < obs_data_array_count(backups); i++) {
		OBSDataAutoRelease item = obs_data_array_item(backups, i);
		OBSDataAutoRelease entry = obs_data_create();
		obs_data_apply(entry, item);
		obs_data_array_push_back(copy, entry);
	}
	return copy;
}

const char *ptz_device_filter_kind(const char *type)
{
	std::string t = type ? type : "";
	if (t == "visca" || t == "visca-over-ip" || t == "visca-over-tcp")
		return "ca.secretlab.obs-ptz.visca";
#if defined(ENABLE_SERIALPORT)
	if (t == "pelco" || t == "pelco-p")
		return "ca.secretlab.obs-ptz.pelco";
#endif
#if defined(ENABLE_ONVIF)
	if (t == "onvif")
		return "ca.secretlab.obs-ptz.onvif";
#endif
#if defined(ENABLE_USB_CAM)
	if (t == "usb-cam")
		return "ca.secretlab.obs-ptz.usb-cam";
#endif
	return nullptr;
}

obs_source_t *ptz_device_create_filter(obs_source_t *parent, obs_data_t *config)
{
	const char *kind = ptz_device_filter_kind(obs_data_get_string(config, "type"));
	if (!parent || !kind)
		return nullptr;

	/* The filter knows its own source, see ptz_filter_save() */
	OBSDataAutoRelease settings = obs_data_create();
	obs_data_apply(settings, config);
	PTZDevice::stripIdentity(settings);
	obs_data_erase(settings, "backup_time");

	QString base = QT_UTF8(obs_source_get_display_name(kind));
	QString name = base;
	for (int i = 2;; i++) {
		OBSSourceAutoRelease existing = obs_source_get_filter_by_name(parent, QT_TO_UTF8(name));
		if (!existing)
			break;
		name = QString("%1 %2").arg(base).arg(i);
	}

	obs_source_t *filter = obs_source_create(kind, QT_TO_UTF8(name), settings, nullptr);
	if (filter)
		obs_source_filter_add(parent, filter);
	return filter;
}

obs_properties_t *ptz_filter_get_properties(void *data)
{
	auto ptz = static_cast<PTZDevice *>(data);
	if (ptz)
		return ptz->get_obs_properties();
	return nullptr;
}

void ptz_filter_update(void *data, obs_data_t *settings)
{
	auto ptz = static_cast<PTZDevice *>(data);
	if (!ptz)
		return;
	/* .update can be called from any thread. We need it on PTZDevice's thread.
	 * Copy the data and use invokeMethod to get to the right thread. The copy
	 * starts from the defaults libobs put in the settings, made into values:
	 * update() takes a complete settings object, but obs_data_apply() copies
	 * only the values that were set, so a plain copy would leave update()
	 * reading 0 for every setting nobody has changed. */
	OBSDataAutoRelease copy = obs_data_get_defaults(settings);
	obs_data_apply(copy, settings);
	OBSData snapshot = copy.Get();
	QMetaObject::invokeMethod(ptz, [ptz, snapshot]() { ptz->applySettings(snapshot); });
}

void *ptz_filter_create(const std::function<PTZDevice *()> &make)
{
	PTZDevice *ptz = nullptr;
	auto build = [&]() {
		ptz = make();
		/* Only announce once the full (base + derived) object is
		 * constructed -- see PTZDevice::announceCreated() */
		ptz->announceCreated();
	};
	/* Creating the device must happen on the main thread */
	if (QThread::currentThread() != qApp->thread())
		QMetaObject::invokeMethod(qApp, build, Qt::BlockingQueuedConnection);
	else
		build();
	return ptz;
}

void ptz_filter_add(void *data, obs_source_t *parent)
{
	auto ptz = static_cast<PTZDevice *>(data);
	if (!ptz)
		return;
	ptz->setParentSource(parent);
	/* Attaching to a source doesn't fire a scene change, so force a refresh */
	QMetaObject::invokeMethod(ptz, [ptz]() { ptz->onSceneChanged(); }, Qt::QueuedConnection);
}

void ptz_filter_remove(void *data, obs_source_t *)
{
	auto ptz = static_cast<PTZDevice *>(data);
	if (ptz) {
		ptz->backup();
		ptz->setParentSource(nullptr);
	}
}

void ptz_filter_destroy(void *data)
{
	auto ptz = static_cast<PTZDevice *>(data);
	if (ptz) {
		ptz->backup();
		ptz->deleteLater();
	}
}

void ptz_filter_save(void *data, obs_data_t *settings)
{
	auto ptz = static_cast<PTZDevice *>(data);
	if (!ptz)
		return;
	ptz->save(settings);
	/* The filter already knows its source, and a device id isn't stable
	 * across a driver change; neither belongs in the scene collection.
	 * Also clears them from collections saved before this was stripped. */
	PTZDevice::stripIdentity(settings);
}

/* C interface for non-QT parts of the plugin */
obs_data_array_t *ptz_devices_get_config()
{
	obs_data_array_t *devices = obs_data_array_create();
	QMutexLocker locker(&ptz_device_registry_mutex);
	for (auto ptz : ptz_device_registry) {
		OBSDataAutoRelease cfg = obs_data_create();
		ptz->save(cfg.Get());
		obs_data_array_push_back(devices, cfg);
	}
	return devices;
}

obs_source_t *ptz_device_get_parent_source(uint32_t device_id)
{
	QMutexLocker locker(&ptz_device_registry_mutex);
	PTZDevice *ptz = ptz_device_registry.value(device_id, nullptr);
	if (!ptz)
		return NULL;
	return ptz->parentSource();
}

void ptz_devices_set_config(obs_data_array_t *devices)
{
	if (!devices) {
		blog(LOG_INFO, "No PTZ device configuration found");
		return;
	}
	for (size_t i = 0; i < obs_data_array_count(devices); i++) {
		OBSData ptzcfg = obs_data_array_item(devices, i);
		obs_data_release(ptzcfg);
		ptz_device_create(ptzcfg);
	}
}

static proc_handler_t *ptz_ph = NULL;
static signal_handler_t *ptz_sh = NULL;

proc_handler_t *ptz_get_proc_handler()
{
	return ptz_ph;
}

signal_handler_t *ptz_get_signal_handler()
{
	return ptz_sh;
}

void ptz_load_devices()
{
	/* Register the proc handlers for issuing PTZ commands */
	ptz_ph = proc_handler_create();
	if (!ptz_ph) {
		blog(LOG_ERROR, "could not allocate proc_handler for PTZ devices");
		return;
	}

	/* Register the signal handler used to announce device creation and destruction */
	ptz_sh = signal_handler_create();
	if (!ptz_sh) {
		blog(LOG_ERROR, "could not allocate signal_handler for PTZ devices");
		return;
	}
	signal_handler_add(ptz_sh, "void ptz_device_create(int device_id, ptr proc_handler, ptr signal_handler)");
	signal_handler_add(ptz_sh, "void ptz_device_destroy(int device_id)");

	/* Constructed here rather than as a plain static-storage global so
	 * its constructor happens at a well-defined point in the module load
	 * instead of at plugin-library-load time -- see PTZListModel::create() */
	PTZListModel::create();

	/* Each backend driver registers its own "<Driver> PTZ Control" OBS filter */
	ptz_visca_register_filter();
	ptz_sony_register_discovery();
#if defined(ENABLE_SERIALPORT)
	ptz_pelco_register_filter();
#endif /* ENABLE_SERIALPORT */
#if defined(ENABLE_ONVIF)
	ptz_onvif_register_filter();
	ptz_onvif_register_discovery();
#endif /* ENABLE_ONVIF */
#if defined(ENABLE_USB_CAM)
	ptz_usb_cam_register_filter();
#endif /* ENABLE_USB_CAM */

	/* Preset Recall/Save Callback */
	auto ptz_cb = [](void *p, calldata_t *cd) {
		ptzDeviceList->callDevice(static_cast<const char *>(p), cd);
	};
	proc_handler_add(ptz_ph, "void ptz_preset_save(int device_id, int preset_id)", ptz_cb,
			 (void *)"ptz_preset_save");
	proc_handler_add(ptz_ph, "void ptz_preset_recall(int device_id, int preset_id)", ptz_cb,
			 (void *)"ptz_preset_recall");
	proc_handler_add(ptz_ph,
			 "void ptz_move_continuous(int device_id, float pan, float tilt, float zoom, float focus)",
			 ptz_cb, (void *)"ptz_move");

	/* Register the new proc hander with the main proc handler */
	proc_handler_t *ph = obs_get_proc_handler();
	if (!ph)
		return;

	/* Register a function for retrieving the PTZ call handler */
	auto ptz_get_proc_handler = [](void *, calldata_t *cd) {
		calldata_set_ptr(cd, "return", ptz_ph);
	};
	proc_handler_add(ph, "ptr ptz_get_proc_handler()", ptz_get_proc_handler, NULL);

	auto ptz_get_api_version = [](void *, calldata_t *cd) {
		calldata_set_int(cd, "major", PTZ_API_VERSION_MAJOR);
		calldata_set_int(cd, "minor", PTZ_API_VERSION_MINOR);
	};
	/* The version of the PTZ API (PTZ_API_VERSION_* in ptz.h), for a caller
	 * to check before it relies on anything else here */
	proc_handler_add(ph, "void ptz_get_api_version(out int major, out int minor)", ptz_get_api_version, NULL);

	/* Deprecated pantilt callback for compatibility with existing plugins */
	proc_handler_add(ph, "void ptz_pantilt(int device_id, float pan, float tilt, float zoom, float focus)", ptz_cb,
			 (void *)"ptz_move");
}

void ptz_unload_devices(void)
{
	/* Reverse of construction order */
	PTZListModel::destroy();
	ptz_discovery_unregister_all();

	{
		QMutexLocker locker(&ptz_backup_mutex);
		ptz_backups = nullptr;
	}
	proc_handler_destroy(ptz_ph);
	ptz_ph = nullptr;
	signal_handler_destroy(ptz_sh);
	ptz_sh = nullptr;
}

void PTZDevice::sanitizePreset(size_t id)
{
	if (!m_presets.contains(id))
		return;
	if (!m_presetsDisplayOrder.contains(id))
		m_presetsDisplayOrder.append(id);
	QVariantMap &preset = m_presets[id];
	QString name = preset["name"].toString();
	if (name == "" || name == QString(obs_module_text("PTZ.PresetNum")).arg(id))
		preset.remove("name");
}

void PTZDevice::setPresetName(size_t id, QString name)
{
	if (!m_presets.contains(id))
		return;
	QVariantMap &preset = m_presets[id];
	preset["name"] = name;
	sanitizePreset(id);

	calldata_t cd = {};
	calldata_set_int(&cd, "device_id", this->id);
	calldata_set_int(&cd, "id", (long long)id);
	signalDevice("preset_renamed", &cd);
	calldata_free(&cd);
}

void PTZDevice::signalPresetThumbnail(size_t id)
{
	calldata_t cd = {};
	calldata_set_int(&cd, "device_id", this->id);
	calldata_set_int(&cd, "id", (long long)id);
	signalDevice("preset_thumbnail_changed", &cd);
	calldata_free(&cd);
}

/* Replace the preset's thumbnail image, removing the old file. Each image
 * gets a fresh file name so nothing caching the old one can go stale. */
void PTZDevice::setPresetThumbnail(size_t id, const QImage &image)
{
	if (!m_presets.contains(id))
		return;
	QString name = ptz_thumbnail_write(image);
	if (name.isEmpty())
		return;
	QVariantMap &preset = m_presets[id];
	ptz_thumbnail_remove(preset.value("thumbnail").toString());
	preset["thumbnail"] = name;
	signalPresetThumbnail(id);
}

void PTZDevice::clearPresetThumbnail(size_t id)
{
	if (!m_presets.contains(id) || presetThumbnail(id).isEmpty())
		return;
	ptz_thumbnail_remove(presetThumbnail(id));
	m_presets[id].remove("thumbnail");
	signalPresetThumbnail(id);
}

void PTZDevice::capturePresetThumbnail(size_t id)
{
	if (!m_presets.contains(id))
		return;
	OBSSourceAutoRelease src = parentSource();
	if (!src)
		return;
	ptz_capture_source_thumbnail(src, this, [this, id](QImage image) { setPresetThumbnail(id, image); });
}

/* Insert a new preset and return the ID */
int PTZDevice::newPreset(int row)
{
	if ((row < 0) || (row > m_presetsDisplayOrder.size()))
		row = m_presetsDisplayOrder.size();
	int id = 0;
	while (m_presets.contains(id))
		id++;
	if (id >= (int)m_maxPresets)
		return -1;

	QVariantMap map;
	map["id"] = (uint)id;
	m_presets[id] = map;
	m_presetsDisplayOrder.insert(row, id);

	calldata_t cd = {};
	calldata_set_int(&cd, "device_id", this->id);
	calldata_set_int(&cd, "row", row);
	signalDevice("preset_inserted", &cd);
	calldata_free(&cd);

	return id;
}

void PTZDevice::removePresetAtDisplayRow(int row)
{
	ptz_thumbnail_remove(presetThumbnail(m_presetsDisplayOrder[row]));
	m_presets.remove(m_presetsDisplayOrder[row]);
	m_presetsDisplayOrder.removeAt(row);

	calldata_t cd = {};
	calldata_set_int(&cd, "device_id", this->id);
	calldata_set_int(&cd, "row", row);
	signalDevice("preset_removed", &cd);
	calldata_free(&cd);
}

/* srcRow/destRow follow QAbstractItemModel::moveRows()'s own convention
 * for destRow (the target index *before* the source row is plucked out)
 * QList::move() wants the index *after* removal, so that adjustment is
 * made here. */
void PTZDevice::movePreset(int srcRow, int destRow)
{
	int listMoveDest = destRow;
	if (srcRow < listMoveDest)
		listMoveDest--;
	m_presetsDisplayOrder.move(srcRow, listMoveDest);

	calldata_t cd = {};
	calldata_set_int(&cd, "device_id", this->id);
	calldata_set_int(&cd, "src_row", srcRow);
	calldata_set_int(&cd, "dest_row", destRow);
	signalDevice("preset_moved", &cd);
	calldata_free(&cd);
}

int PTZDevice::presetAtDisplayRow(int row) const
{
	if (row < 0 || row >= presetCount())
		return -1;
	return (int)m_presetsDisplayOrder[row];
}

QVariant PTZDevice::presetProperty(size_t id, QString key) const
{
	/* Safe to dereference unconditionally here. Both levels will return
	 * default empty values without segfaulting */
	return m_presets[id][key];
}

bool PTZDevice::updatePreset(size_t id, const QVariantMap &map)
{
	if (!m_presets.contains(id))
		return false;
	m_presets[id].insert(map);
	return true;
}

int PTZDevice::findPreset(QString key, QVariant value) const
{
	/* Search for the matching key/value pair in all presets.
	 * This is an O(N) operation */
	auto end = m_presets.cend();
	for (auto i = m_presets.cbegin(); i != end; i++) {
		if (i.value()[key] == value)
			return (int)i.key();
	}
	return -1;
}

void PTZDevice::incrementStatistic(const char *name)
{
	obs_data_set_int(statistics, name, obs_data_get_int(statistics, name) + 1);
}

void PTZDevice::setConnected(bool _connected)
{
	if (wrongThread("setConnected"))
		return;
	if (connected == _connected)
		return;
	connected = _connected;
	obs_data_set_bool(stateChanged, "connected", connected);
	notifyStateChanged();
}

void PTZDevice::setLock(bool state)
{
	if (locked == state)
		return;
	locked = state;
	obs_data_set_bool(stateChanged, "locked", locked);
	notifyStateChanged();
}

/**
 * Fires one of sigs' own signals. For a filter-owned device, first grab
 * a strong reference to the filter to guarantee the signal handler is
 * valid. Otherwise the filter could be destroyed in parallel, risking a
 * use-after-free. */
void PTZDevice::signalDevice(const char *name, calldata_t *cd)
{
	OBSSourceAutoRelease filter = obs_weak_source_get_source(m_filter);
	if (filter || isSelfManaged())
		signal_handler_signal(sigs, name, cd);
}

void PTZDevice::notifyStateChanged()
{
	calldata_t cd = {};
	calldata_set_int(&cd, "device_id", id);
	calldata_set_ptr(&cd, "changed", stateChanged);
	signalDevice("state_changed", &cd);
	calldata_free(&cd);
	/* Notification done. Start a new object for what changes next, rather
	 * than clearing this one: a listener may keep a reference to what it
	 * was told changed. */
	OBSDataAutoRelease next = obs_data_create();
	stateChanged = next.Get();
}

bool PTZDevice::wrongThread(const char *method) const
{
	if (QThread::currentThread() == thread())
		return false;
	ptz_log(LOG_ERROR, "%s called from wrong thread; ignored", method);
	return true;
}
