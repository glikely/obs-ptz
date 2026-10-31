/* Pan Tilt Zoom Controls - PTZDevice base object
 *
 * Copyright 2020-2026 Grant Likely <grant.likely@secretlab.ca>
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include <obs.hpp>
#include <algorithm>
#include <functional>
#include <vector>
#include <QByteArray>
#include <QCoreApplication>
#include <QFile>
#include <QFileInfo>
#include <QHash>
#include <QSet>
#include <QJsonDocument>
#include <QMutex>
#include <QThread>
#include <QTimer>
#include <QUrl>
#include <QUuid>
#include "ptz-device.hpp"
#include "ptz-thumbnail-files.hpp"
#include "ptz.h"
#include "protocol-helpers.hpp"

/* What this module has registered, by scope, for ptz_get_api_info: libobs
 * can't be asked what a handler has, and a caller outside the module has no
 * other way to learn a declaration. Each is recorded once, the first time it
 * is registered, so it is whole by the time the first device or discovery
 * instance exists to be asked, and only read from then on. */
struct PTZApiEntry {
	QByteArray decl;
	PTZProcThread thread;
	PTZProcAccess access;
};
static QMutex ptz_api_mutex;
static QMap<QByteArray, QList<PTZApiEntry>> ptz_api_recorded;

static void ptz_api_record(const char *scope, const char *decl, PTZProcThread thread, PTZProcAccess access)
{
	QMutexLocker locker(&ptz_api_mutex);
	QList<PTZApiEntry> &entries = ptz_api_recorded[scope];
	for (const PTZApiEntry &entry : entries)
		if (entry.decl == decl)
			return;
	entries.append({decl, thread, access});
}

/* The name in a declaration, "void ptz_stop()": what is before the bracket */
static QByteArray ptz_api_decl_name(const QByteArray &decl)
{
	int bracket = decl.indexOf('(');
	int space = decl.lastIndexOf(' ', bracket);
	return bracket < 0 ? QByteArray() : decl.mid(space + 1, bracket - space - 1);
}

void ptz_get_api_info_proc(void *data, calldata_t *cd)
{
	QByteArray kind = static_cast<const char *>(data);
	obs_data_t *info = obs_data_create();
	QMutexLocker locker(&ptz_api_mutex);
	OBSDataArrayAutoRelease procs = obs_data_array_create();
	for (const PTZApiEntry &entry : ptz_api_recorded.value(kind + "-proc")) {
		OBSDataAutoRelease item = obs_data_create();
		obs_data_set_string(item, "name", ptz_api_decl_name(entry.decl).constData());
		obs_data_set_string(item, "decl", entry.decl.constData());
		obs_data_set_string(item, "thread", entry.thread == PTZ_THREAD_ANY ? "any" : "device");
		obs_data_set_string(item, "access",
				    entry.access == PTZ_ACCESS_READ   ? "read"
				    : entry.access == PTZ_ACCESS_NONE ? "none"
								      : "control");
		obs_data_array_push_back(procs, item);
	}
	OBSDataArrayAutoRelease signalList = obs_data_array_create();
	for (const PTZApiEntry &entry : ptz_api_recorded.value(kind + "-signal")) {
		OBSDataAutoRelease item = obs_data_create();
		obs_data_set_string(item, "name", ptz_api_decl_name(entry.decl).constData());
		obs_data_set_string(item, "decl", entry.decl.constData());
		obs_data_array_push_back(signalList, item);
	}
	obs_data_set_array(info, "procs", procs);
	obs_data_set_array(info, "signals", signalList);
	calldata_set_ptr(cd, "return", info);
}

void ptz_proc_add(const char *scope, proc_handler_t *handler, const char *decl, proc_handler_proc_t proc, void *data,
		  PTZProcThread thread, PTZProcAccess access)
{
	ptz_api_record(scope, decl, thread, access);
	proc_handler_add(handler, decl, proc, data);
}

void ptz_signal_add(const char *scope, signal_handler_t *handler, const char *decl)
{
	ptz_api_record(scope, decl, PTZ_THREAD_DEVICE, PTZ_ACCESS_NONE);
	signal_handler_add(handler, decl);
}

void PTZDevice::addProc(const char *decl, proc_handler_proc_t proc, void *data, PTZProcThread thread,
			PTZProcAccess access)
{
	ptz_proc_add("device-proc", handler, decl, proc, data, thread, access);
}

void PTZDevice::addSignal(const char *decl)
{
	ptz_signal_add("device-signal", sigs, decl);
}

void ptz_get_api_version_proc(void *, calldata_t *cd)
{
	calldata_set_int(cd, "major", PTZ_API_VERSION_MAJOR);
	calldata_set_int(cd, "minor", PTZ_API_VERSION_MINOR);
}

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

/* This module's devices, for its one frontend callback below (main thread only) */
static PTZLifecycle &module_lifecycle()
{
	static PTZLifecycle lifecycle;
	return lifecycle;
}

static std::vector<PTZDevice *> &module_devices()
{
	static std::vector<PTZDevice *> devices;
	return devices;
}

/* The axes a move can name, which each device has one of in m_axes, and the
 * labels of the settings each has */
struct AxisInfo {
	const char *name;
	const char *speedMaxText;
	const char *invertText;
};
static constexpr AxisInfo kAxes[] = {
	{"pan", "PTZ.Device.PanMaxSpeed", "PTZ.Device.PanInvertAxis"},
	{"tilt", "PTZ.Device.TiltMaxSpeed", "PTZ.Device.TiltInvertAxis"},
	{"zoom", "PTZ.Device.ZoomMaxSpeed", "PTZ.Device.ZoomInvertAxis"},
	{"focus", "PTZ.Device.FocusMaxSpeed", "PTZ.Device.FocusInvertAxis"},
};

/* An axis's setting: "<axis>_<setting>", as "pan_speed_max" */
static std::string settingKey(const char *axis, const char *setting)
{
	return std::string(axis) + "_" + setting;
}

PTZDevice::PTZDevice(OBSData config, obs_source_t *filter) : QObject()
{
	m_filter = OBSGetWeakRef(filter);
	upgradeSettings(config);
	for (const AxisInfo &info : kAxes)
		m_axes.push_back({info.name});

	/* proc_handers for calling into the device. Comes from its filter */
	handler = obs_source_get_proc_handler(filter);
	if (!handler) {
		blog(LOG_ERROR, "could not allocate proc_handler for a PTZ device");
		return;
	}

	/* The device's own version: see docs/ptz-device-api.md */
	addProc("void ptz_get_api_version(out int major, out int minor)", ptz_get_api_version_proc, nullptr,
		PTZ_THREAD_ANY, PTZ_ACCESS_READ);
	/* And what it has, for a caller that can't know: see ptz_get_api_info_proc() */
	addProc("ptr ptz_get_api_info()", ptz_get_api_info_proc, const_cast<char *>("device"), PTZ_THREAD_ANY,
		PTZ_ACCESS_READ);

	/* The PTZ Device API, which docs/ptz-device-api.md specifies and
	 * tests/obs-integration/test_api_doc.py holds to what is registered
	 * here. All these functions are prefixed with 'ptz_' so that they can
	 * be added to an existing proc_handler with low risk of conflicts */
	addProc("void ptz_stop()", ptz_ph_lambda(stop), this, PTZ_THREAD_ANY);
	addProc("void ptz_home_recall()", ptz_ph_lambda(pantilt_home), this, PTZ_THREAD_ANY);
	addProc("void ptz_home_save()", ptz_ph_lambda(pantilt_set_home), this, PTZ_THREAD_ANY);
	addProc("void ptz_move(float pan, float tilt, float zoom, float focus)", ptz_ph_lambda(move), this,
		PTZ_THREAD_ANY);
	addProc("void ptz_move_abs(float pan, float tilt, float zoom, float focus)", ptz_ph_lambda(move_abs), this,
		PTZ_THREAD_ANY);
	addProc("void ptz_move_rel(float pan, float tilt)", ptz_ph_lambda(move_rel), this, PTZ_THREAD_ANY);
	addProc("void ptz_preset_save(string id)", ptz_ph_lambda(preset_save), this, PTZ_THREAD_ANY);
	addProc("void ptz_preset_recall(string id)", ptz_ph_lambda(preset_recall), this, PTZ_THREAD_ANY);
	addProc("void ptz_preset_capture_thumbnail(string id, bool if_arrived)",
		ptz_ph_lambda(preset_capture_thumbnail), this, PTZ_THREAD_ANY);

	addProc("ptr ptz_get_state(ptr state)", ptz_ph_lambda(get_state), this, PTZ_THREAD_ANY, PTZ_ACCESS_READ);
	addProc("ptr ptz_get_statistics(ptr statistics)", ptz_ph_lambda(get_statistics), this, PTZ_THREAD_DEVICE,
		PTZ_ACCESS_READ);
	addProc("ptr ptz_get_parent_source()", ptz_ph_lambda(get_parent_source), this, PTZ_THREAD_ANY, PTZ_ACCESS_NONE);

	addProc("void ptz_request_state(ptr state)", ptz_ph_lambda(request_state), this);

	addProc("void ptz_trigger(string name)", ptz_ph_lambda(trigger), this, PTZ_THREAD_ANY);

	addProc("void ptz_get_camera_report(out string report)", ptz_ph_lambda(get_camera_report), this,
		PTZ_THREAD_DEVICE, PTZ_ACCESS_READ);

	addProc("ptr ptz_preset_get_list()", ptz_ph_lambda(preset_get_list), this, PTZ_THREAD_DEVICE, PTZ_ACCESS_READ);
	addProc("void ptz_preset_get_thumbnail(string id, out string mime, out string data)",
		ptz_ph_lambda(preset_get_thumbnail), this, PTZ_THREAD_DEVICE, PTZ_ACCESS_READ);
	addProc("string ptz_preset_create(string name, string store)", ptz_ph_lambda(preset_create), this);
	addProc("void ptz_preset_delete(string id)", ptz_ph_lambda(preset_delete), this);
	addProc("void ptz_preset_update(string id, ptr changes)", ptz_ph_lambda(preset_update), this);
	addProc("void ptz_preset_move(string id, int index)", ptz_ph_lambda(preset_move), this);
	addProc("void ptz_preset_refresh()", ptz_ph_lambda(preset_refresh), this);

	/* Signal handler for notifying state & settings changes. Shared with
	 * the filter, same as handler above. */
	sigs = obs_source_get_signal_handler(filter);
	if (!sigs) {
		blog(LOG_ERROR, "could not allocate signal_handler for a PTZ device");
	} else {
		addSignal("void ptz_state_changed(ptr source, ptr changed)");

		addSignal("void ptz_preset_added(ptr source, string id)");
		addSignal("void ptz_preset_removed(ptr source, string id)");
		addSignal("void ptz_preset_order_changed(ptr source)");
		addSignal("void ptz_preset_changed(ptr source, string id, ptr changed)");
		addSignal("void ptz_preset_list_reset(ptr source)");
		addSignal("void ptz_preset_create_done(ptr source, string request, string id)");
		addSignal("void ptz_preset_recalled(ptr source, string id)");
	}

	/* The device is given its source by its filter, see setParentSource() */
	type = obs_data_get_string(config, "type");
	state = obs_data_create();
	obs_data_release(state);
	stateChanged = obs_data_create();
	obs_data_release(stateChanged);
	statistics = obs_data_create();
	obs_data_release(statistics);
	stale_state = {"pan_pos", "tilt_pos", "zoom_pos", "focus_pos"};

	/* Hear of OBS finishing loading and closing from this module's one
	 * frontend callback (ptz_module_frontend_event()), not the UI. A device is made
	 * on the main thread, where OBS sends them; if loading has finished
	 * already this starts it. */
	module_devices().push_back(this);
	module_lifecycle().add(this);
}

void PTZDevice::onStartup()
{
	/* Queued: a device being constructed has not finished being a subclass,
	 * and the driver's hook runs on the device's own thread. Dropped if the
	 * device is gone first. */
	QMetaObject::invokeMethod(this, [this]() { onOBSStartup(); }, Qt::QueuedConnection);
}

void PTZDevice::onShutdown()
{
	/* OBS is closing, and has not yet cleared its scenes, which destroys the
	 * filters that own devices: EXIT comes after that. This is the last time
	 * every device is still there, and what runs must not wait for the
	 * event loop. OBS sends it on the main thread, which is the device's:
	 * blocking on another thread here deadlocks at exit (commit caba7fc). */
	onOBSShutdown();
}

PTZDevice::~PTZDevice()
{
	module_lifecycle().remove(this);
	auto &devices = module_devices();
	devices.erase(std::remove(devices.begin(), devices.end(), this), devices.end());

	/* Stop watching the source */
	watchParentSource(m_parentSource, false);

	/* The handlers are the filter's, which frees them */
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
		signal_handler_connect(sh, "update", ptz_source_updated_cb, self);
		QMetaObject::invokeMethod(self, [self]() { self->checkParentHost(); }, Qt::QueuedConnection);
	} else {
		signal_handler_disconnect(sh, "rename", ptz_source_renamed_cb, self);
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
	if (!id)
		return QString();
	OBSDataAutoRelease settings = obs_source_get_settings(src);
	QString url;
	if (strcmp(id, "ndi_source") == 0) {
		url = QT_UTF8(obs_data_get_string(settings, "web_control_url"));
	} else if (strcmp(id, "browser_source") == 0) {
		/* A page from a file has no host, and a new source starts on
		 * OBS's own page, which isn't a camera */
		if (obs_data_get_bool(settings, "is_local_file"))
			return QString();
		url = QT_UTF8(obs_data_get_string(settings, "url"));
		if (url == QStringLiteral("https://obsproject.com/browser-source"))
			return QString();
	}
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
	/* The host is the placeholder of a blank Host, in the settings */
	publish();
	settingsChanged();
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
	obs_data_set_string(stateChanged, "source", QT_TO_UTF8(name));
	notifyStateChanged();
}

/* The name of the source the device is on, or the last it was on while it is
 * on none: "" if it never has been */
QString PTZDevice::sourceName() const
{
	OBSSourceAutoRelease src = parentSource();
	QMutexLocker locker(&m_parentSourceMutex);
	return src ? QT_UTF8(obs_source_get_name(src)) : m_parentSourceName;
}

void PTZDevice::onSceneChanged()
{
	if (!tally_auto || !features().testFlag(TallyLight))
		return;
	bool live = false, preview = false;
	OBSSourceAutoRelease source = parentSource();
	if (source) {
		OBSSourceAutoRelease program = obs_frontend_get_current_scene();
		live = ptz_scene_is_source_active(program, source);
		if (obs_frontend_preview_program_mode_active()) {
			OBSSourceAutoRelease previewScene = obs_frontend_get_current_preview_scene();
			preview = ptz_scene_is_source_active(previewScene, source);
		}
	}
	bool green = preview && !live;
	bool programChanged = live != m_tallyProgram, previewChanged = green != m_tallyPreview;
	m_tallyProgram = live;
	m_tallyPreview = green;
	if (programChanged)
		setTally(Tally::Program, live);
	if (previewChanged)
		setTally(Tally::Preview, green);
}

static size_t dataCount(obs_data_t *data);

PTZDevice::Axis *PTZDevice::axis(const char *name)
{
	for (Axis &a : m_axes) {
		if (!strcmp(a.name, name))
			return &a;
	}
	return nullptr;
}

const PTZDevice::Axis *PTZDevice::axis(const char *name) const
{
	return const_cast<PTZDevice *>(this)->axis(name);
}

double PTZDevice::speed(const char *name) const
{
	const Axis *a = axis(name);
	return a ? a->speed : 0;
}

void PTZDevice::stop()
{
	OBSDataAutoRelease speeds = obs_data_create();
	for (const Axis &a : m_axes)
		obs_data_set_double(speeds, a.name, 0);
	drive(speeds.Get());
}

void PTZDevice::drive(OBSData speeds)
{
	OBSDataAutoRelease changed = obs_data_create();
	for (Axis &a : m_axes) {
		if (!obs_data_has_user_value(speeds, a.name))
			continue;
		double s =
			std::clamp(obs_data_get_double(speeds, a.name) * (a.invert ? -1 : 1), -a.speedMax, a.speedMax);
		if (a.speed == s)
			continue;
		a.speed = s;
		obs_data_set_double(changed, a.name, s);
	}
	if (dataCount(changed))
		moveDrive(changed.Get());
}

/* Each movement proc drops a pending recall's report, queued ahead of the move
 * so the two run in order on the device's thread */
void PTZDevice::stop(calldata_t *)
{
	QMetaObject::invokeMethod(this, [this] { cancelRecall(); });
	QMetaObject::invokeMethod(this, "stop");
}

void PTZDevice::pantilt_home(calldata_t *)
{
	QMetaObject::invokeMethod(this, [this] { cancelRecall(); });
	QMetaObject::invokeMethod(this, "pantilt_home");
}

OBSData PTZDevice::moveValues(calldata_t *cd) const
{
	OBSDataAutoRelease values = obs_data_create();
	double v;
	for (const Axis &a : m_axes)
		if (calldata_get_float(cd, a.name, &v))
			obs_data_set_double(values, a.name, v);
	return values.Get();
}

void PTZDevice::move(calldata_t *cd)
{
	OBSData speeds = moveValues(cd);

	/* Only a speed: a joystick says 0 over and over while it is idle, which
	 * moves nothing. Stopping a real move cancelled with its start. */
	bool moving = false;
	for (obs_data_item_t *item = obs_data_first(speeds); item; obs_data_item_next(&item))
		moving |= obs_data_item_get_double(item) != 0;
	if (moving)
		QMetaObject::invokeMethod(this, [this] { cancelRecall(); });

	QMetaObject::invokeMethod(this, [this, speeds] { drive(speeds); });
}

void PTZDevice::move_abs(calldata_t *cd)
{
	OBSData positions = moveValues(cd);
	QMetaObject::invokeMethod(this, [this, positions] {
		cancelRecall();
		pairPanTilt(positions);
		if (dataCount(positions))
			moveAbs(positions);
	});
}

void PTZDevice::move_rel(calldata_t *cd)
{
	OBSData distances = moveValues(cd);
	QMetaObject::invokeMethod(this, [this, distances] {
		cancelRecall();
		if (dataCount(distances))
			moveRel(distances);
	});
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
	obs_data_set_bool(out, "connected", connected);
	obs_data_set_string(out, "source", QT_TO_UTF8(sourceName()));
	saveFeatures(out, features());
}

const QList<QPair<PTZDevice::Feature, const char *>> &PTZDevice::featureNames()
{
	static const QList<QPair<Feature, const char *>> names = {
		{Pan, "pan"},
		{Tilt, "tilt"},
		{Zoom, "zoom"},
		{Focus, "focus"},
		{PanAbs, "pan_abs"},
		{TiltAbs, "tilt_abs"},
		{PanRel, "pan_rel"},
		{TiltRel, "tilt_rel"},
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
		{TallyLight, "tally_light"},
		{MoveCompletion, "move_completion"},
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
	/* "presets" is either store: the local one needs only a position to go to */
	if (features & (PanAbs | TiltAbs | ZoomAbs | FocusAbs))
		obs_data_set_bool(names, "presets", true);
	/* "pantilt" and the rest are both axes', as they were before each had its own */
	const QList<QPair<Features, const char *>> pairs = {
		{Pan | Tilt, "pantilt"},
		{PanAbs | TiltAbs, "pantilt_abs"},
		{PanRel | TiltRel, "pantilt_rel"},
	};
	for (const auto &[both, name] : pairs) {
		if ((features & both) == both)
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
	/* A camera found to have lamps is lit for what its source is in */
	onSceneChanged();
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

/* One axis's entry of "axes", in the state or in what changed, made if it isn't there */
static OBSData axisEntry(obs_data_t *parent, const char *axis)
{
	OBSDataAutoRelease axes = obs_data_get_obj(parent, "axes");
	if (!axes) {
		axes = obs_data_create();
		obs_data_set_obj(parent, "axes", axes);
	}
	OBSDataAutoRelease entry = obs_data_get_obj(axes, axis);
	if (!entry) {
		entry = obs_data_create();
		obs_data_set_obj(axes, axis, entry);
	}
	return OBSData(entry.Get());
}

bool PTZDevice::setAxisStep(const char *name, double step)
{
	Axis *a = axis(name);
	if (!a || !(step > 0.0))
		return false;
	step = std::min(step, 1.0);
	if (fabs(a->step - step) <= step * 1e-6)
		return false;
	a->step = step;
	for (obs_data_t *where : {state.Get(), stateChanged.Get()})
		obs_data_set_double(axisEntry(where, name), "step", step);
	return true;
}

bool PTZDevice::setAxisScale(const char *name, const char *unit, const std::vector<AxisPoint> &points)
{
	Axis *a = axis(name);
	if (!a || !unit || !*unit || points.size() < 2)
		return false;
	OBSDataArrayAutoRelease array = obs_data_array_create();
	for (const AxisPoint &point : points) {
		OBSDataAutoRelease item = obs_data_create();
		obs_data_set_double(item, "position", point.position);
		if (point.value)
			obs_data_set_double(item, "value", *point.value);
		obs_data_array_push_back(array, item);
	}
	/* Said again only when it differs, by how it is written: its JSON */
	OBSDataAutoRelease written = obs_data_create();
	obs_data_set_string(written, "unit", unit);
	obs_data_set_array(written, "points", array);
	std::string json = obs_data_get_json(written);
	if (a->scale == json)
		return false;
	a->scale = json;
	for (obs_data_t *where : {state.Get(), stateChanged.Get()}) {
		OBSData entry = axisEntry(where, name);
		obs_data_set_string(entry, "unit", unit);
		obs_data_set_array(entry, "points", array);
	}
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
	const char *id = nullptr;
	if (calldata_get_string(cd, "id", &id) && id) {
		/* The proc_handler can be called from any thread, m_presets can't */
		QString preset = QString::fromUtf8(id);
		QMetaObject::invokeMethod(this, [this, preset] { savePreset(preset); });
	}
}

void PTZDevice::preset_recall(calldata_t *cd)
{
	const char *id = nullptr;
	if (calldata_get_string(cd, "id", &id) && id) {
		QString preset = QString::fromUtf8(id);
		QMetaObject::invokeMethod(this, [this, preset] { recallPreset(preset); });
	}
}

/* With "if_arrived", only while the preset is still where the last recall
 * arrived: not once the camera has been sent elsewhere */
void PTZDevice::preset_capture_thumbnail(calldata_t *cd)
{
	const char *id = nullptr;
	if (calldata_get_string(cd, "id", &id) && id) {
		QString preset = QString::fromUtf8(id);
		bool ifArrived = false;
		calldata_get_bool(cd, "if_arrived", &ifArrived);
		QMetaObject::invokeMethod(this, [this, preset, ifArrived] {
			if (!ifArrived || preset == m_arrivedId)
				capturePresetThumbnail(preset);
		});
	}
}

void PTZDevice::refreshStateSnapshot()
{
	OBSDataAutoRelease whole = obs_data_create();
	saveState(whole.Get());
	std::string json = obs_data_get_json(whole);
	QMutexLocker locker(&m_stateSnapshotMutex);
	m_stateSnapshot = std::move(json);
}

/**
 * Fills the caller-owned obs_data_t with the device's whole transient state.
 * On the device's own thread that is the state itself. On any other, which
 * must not touch it, it is the copy made when the state was last announced,
 * which is as new as anything a listener has been told.
 */
void PTZDevice::get_state(calldata_t *cd) const
{
	auto state = static_cast<obs_data_t *>(calldata_ptr(cd, "state"));
	if (!state)
		return;
	if (QThread::currentThread() == thread()) {
		saveState(state);
		return;
	}

	std::string json;
	{
		QMutexLocker locker(&m_stateSnapshotMutex);
		json = m_stateSnapshot;
	}
	/* Read back as text, so that what the caller has shares nothing with the
	 * device's state, or with another caller's: nested objects are shared by
	 * obs_data_apply(), and the caller may change what it was given */
	OBSDataAutoRelease copy = json.empty() ? nullptr : obs_data_create_from_json(json.c_str());
	if (copy)
		obs_data_apply(state, copy);
}

/**
 * Fills the caller-owned obs_data_t with the device's statistics
 */
void PTZDevice::get_statistics(calldata_t *cd)
{
	if (wrongThread("ptz_get_statistics"))
		return;
	auto out = static_cast<obs_data_t *>(calldata_ptr(cd, "statistics"));
	if (out)
		saveStatistics(out);
}

void PTZDevice::saveStatistics(OBSData out)
{
	obs_data_apply(out, statistics);
}

/* The driver's report, after what describes every report: what made it, on
 * which kind of computer, and what kind of device it is */
void PTZDevice::get_camera_report(calldata_t *cd) const
{
	if (wrongThread("ptz_get_camera_report"))
		return;
	QJsonObject report = cameraReport();
	if (report.isEmpty()) {
		calldata_set_string(cd, "report", "");
		return;
	}
#if defined(_WIN32)
	const char *os = "Windows";
#elif defined(__APPLE__)
	const char *os = "macOS";
#else
	const char *os = "Linux";
#endif
	report.insert("report", "obs-ptz camera report");
	report.insert("format", 1);
	report.insert("plugin_version", ptz_plugin_version);
	report.insert("os", os);
	report.insert("type", QString::fromStdString(type));
	calldata_set_string(cd, "report", QJsonDocument(report).toJson(QJsonDocument::Indented).constData());
}

void PTZDevice::get_parent_source(calldata_t *cd) const
{
	/* Any thread: parentSource() has its own lock */
	calldata_set_ptr(cd, "return", parentSource());
}

/**
 * Returns, as "return", a caller-owned obs_data_t that describes the presets:
 * them in display order, and what stores and capabilities the device has. See
 * docs/ptz-device-api.md.
 */
void PTZDevice::preset_get_list(calldata_t *cd) const
{
	if (wrongThread("ptz_preset_get_list"))
		return;
	obs_data_t *info = obs_data_create();
	obs_data_t *list = obs_data_create();
	for (const Preset &preset : m_presets) {
		obs_data_t *item = obs_data_create();
		obs_data_set_string(item, "id", QT_TO_UTF8(preset.id));
		obs_data_set_string(item, "store", preset.onCamera() ? "camera" : "local");
		obs_data_set_string(item, "name", QT_TO_UTF8(preset.name));
		obs_data_set_string(item, "camera_name", QT_TO_UTF8(preset.cameraName));
		obs_data_set_string(item, "thumbnail", QT_TO_UTF8(ptz_thumbnail_path(preset.thumbnail)));
		if (!preset.onCamera()) {
			OBSDataAutoRelease values = obs_data_create();
			obs_data_apply(values, preset.values);
			obs_data_set_obj(item, "values", values);
		}
		obs_data_set_obj(list, QT_TO_UTF8(preset.id), item);
		obs_data_release(item);
	}
	obs_data_set_obj(info, "presets", list);
	obs_data_release(list);
	obs_data_array_t *order = obs_data_array_create();
	for (const QString &id : m_order) {
		if (presetIndex(id) < 0)
			continue;
		obs_data_t *entry = obs_data_create();
		obs_data_set_string(entry, "id", QT_TO_UTF8(id));
		obs_data_array_push_back(order, entry);
		obs_data_release(entry);
	}
	obs_data_set_array(info, "order", order);
	obs_data_array_release(order);

	CameraPresets camera = cameraPresets();
	obs_data_t *stores = obs_data_create();
	if (camera.available)
		obs_data_set_bool(stores, "camera", true);
	if (localPresets())
		obs_data_set_bool(stores, "local", true);
	obs_data_set_obj(info, "stores", stores);
	obs_data_release(stores);
	obs_data_t *values = obs_data_create();
	for (const QString &key : valueKeys())
		obs_data_set_bool(values, QT_TO_UTF8(key), true);
	obs_data_set_obj(info, "value_keys", values);
	obs_data_release(values);
	obs_data_set_bool(info, "names_on_camera", camera.namesOnCamera);
	obs_data_set_bool(info, "enumerable", camera.enumerable);
	calldata_set_ptr(cd, "return", info);
}

/**
 * Returns the image of a preset's thumbnail, for a caller that can't read the
 * file the list names: its type as "mime" and the file, base64 encoded, as
 * "data". Both are "" for a preset with no thumbnail or none that can be read.
 */
void PTZDevice::preset_get_thumbnail(calldata_t *cd) const
{
	if (wrongThread("ptz_preset_get_thumbnail"))
		return;
	const char *id = nullptr;
	calldata_get_string(cd, "id", &id);
	QString mime;
	QByteArray data;
	int index = id ? presetIndex(QString::fromUtf8(id)) : -1;
	if (index >= 0 && !m_presets.at(index).thumbnail.isEmpty()) {
		QString path = ptz_thumbnail_path(m_presets.at(index).thumbnail);
		QFile file(path);
		if (file.open(QIODevice::ReadOnly)) {
			data = file.readAll().toBase64();
			mime = QFileInfo(path).suffix().toLower() == "png" ? "image/png" : "image/jpeg";
		}
	}
	calldata_set_string(cd, "mime", QT_TO_UTF8(mime));
	calldata_set_string(cd, "data", data.constData());
}

void PTZDevice::preset_create(calldata_t *cd)
{
	if (wrongThread("ptz_preset_create"))
		return;
	const char *name = "";
	const char *store = "";
	calldata_get_string(cd, "name", &name);
	calldata_get_string(cd, "store", &store);
	QString request = createPreset(QString::fromUtf8(name ? name : ""), QString::fromUtf8(store ? store : ""));
	calldata_set_string(cd, "return", QT_TO_UTF8(request));
}

void PTZDevice::preset_delete(calldata_t *cd)
{
	if (wrongThread("ptz_preset_delete"))
		return;
	const char *id = nullptr;
	if (calldata_get_string(cd, "id", &id) && id)
		deletePreset(QString::fromUtf8(id));
}

void PTZDevice::preset_update(calldata_t *cd)
{
	if (wrongThread("ptz_preset_update"))
		return;
	const char *id = nullptr;
	auto changes = static_cast<obs_data_t *>(calldata_ptr(cd, "changes"));
	if (calldata_get_string(cd, "id", &id) && id && changes)
		updatePreset(QString::fromUtf8(id), changes);
}

void PTZDevice::preset_move(calldata_t *cd)
{
	if (wrongThread("ptz_preset_move"))
		return;
	const char *id = nullptr;
	if (calldata_get_string(cd, "id", &id) && id)
		movePreset(QString::fromUtf8(id), (int)calldata_int(cd, "index"));
}

void PTZDevice::preset_refresh(calldata_t *)
{
	if (wrongThread("ptz_preset_refresh"))
		return;
	cameraPresetRefresh();
}

void PTZDevice::defaults(obs_data_t *config)
{
	for (const AxisInfo &info : kAxes) {
		obs_data_set_default_double(config, settingKey(info.name, "speed_max").c_str(), 1.0);
		obs_data_set_default_bool(config, settingKey(info.name, "invert").c_str(), false);
	}
	obs_data_set_default_bool(config, "tally_auto", true);
}

void PTZDevice::applySettings(OBSData settings)
{
	update(settings);
	/* A new type can be a new kind of transport, with another host to say */
	publish();
}

/* A change the device made to its own settings, which OBS says the way it does
 * any other: its source updated. The device is updated with them again, which
 * leaves them as they are. */
void PTZDevice::settingsChanged()
{
	OBSSourceAutoRelease filter = filterSource();
	if (filter)
		obs_source_update(filter, nullptr);
}

void PTZDevice::stripIdentity(obs_data_t *settings)
{
	/* Written by versions that kept them in the settings */
	obs_data_erase(settings, "name");
	obs_data_erase(settings, "id");
	/* Written by versions that still had self-managed devices */
	obs_data_erase(settings, "is-self-managed");
}

void PTZDevice::upgradeSettings(obs_data_t *settings)
{
	/* One cap for both, before each had its own. A cap set since is newer,
	 * and kept. */
	if (obs_data_has_user_value(settings, "pantilt_speed_max")) {
		double max = obs_data_get_double(settings, "pantilt_speed_max");
		for (const char *key : {"pan_speed_max", "tilt_speed_max"}) {
			if (!obs_data_has_user_value(settings, key))
				obs_data_set_double(settings, key, max);
		}
		obs_data_erase(settings, "pantilt_speed_max");
	}
}

void PTZDevice::update(OBSData config)
{
	/* Clamp to the same range enforced by the properties slider; a corrupt
	 * or hand-edited config must not yield an absurd preset count. Only a
	 * driver with slotPresets() uses it, and the others do not save it. */
	m_maxPresets = std::clamp<size_t>(obs_data_get_int(config, "preset_max"), 1, 128);
	OBSDataArrayAutoRelease preset_array = obs_data_get_array(config, "presets");
	OBSDataArrayAutoRelease order_array = obs_data_get_array(config, "preset_order");
	loadPresets(preset_array, order_array);

	bool was_tally_auto = tally_auto;
	tally_auto = obs_data_get_bool(config, "tally_auto");
	/* Turned on, it lights the lamps for what the source is in now, which
	 * is for the device's own thread, with the device's driver finished
	 * with its own update */
	if (tally_auto && !was_tally_auto)
		QMetaObject::invokeMethod(this, [this]() { onSceneChanged(); }, Qt::QueuedConnection);

	for (Axis &a : m_axes) {
		a.speedMax = obs_data_get_double(config, settingKey(a.name, "speed_max").c_str());
		a.invert = obs_data_get_bool(config, settingKey(a.name, "invert").c_str());
	}
}

void PTZDevice::save(OBSData config) const
{
	obs_data_set_string(config, "type", type.c_str());
	for (const Axis &a : m_axes) {
		obs_data_set_double(config, settingKey(a.name, "speed_max").c_str(), a.speedMax);
		obs_data_set_bool(config, settingKey(a.name, "invert").c_str(), a.invert);
	}
	obs_data_set_bool(config, "tally_auto", tally_auto);
	if (slotPresets())
		obs_data_set_int(config, "preset_max", m_maxPresets);
	persistState(config);
	saveDefaults(config);
}

/* How many keys an obs_data_t has */
static size_t dataCount(obs_data_t *data)
{
	size_t count = 0;
	for (obs_data_item_t *item = obs_data_first(data); item; obs_data_item_next(&item))
		count++;
	return count;
}

void PTZDevice::persistState(obs_data_t *settings) const
{
	/* What the user added to a camera's preset, and what is needed to know a
	 * camera that can't be asked has it: not what the camera itself has */
	CameraPresets camera = cameraPresets();
	OBSDataArrayAutoRelease preset_array = obs_data_array_create();
	for (const Preset &preset : m_presets) {
		OBSDataAutoRelease data = obs_data_create();
		obs_data_set_string(data, "id", QT_TO_UTF8(preset.id));
		if ((!preset.onCamera() || !camera.namesOnCamera) && !preset.name.isEmpty())
			obs_data_set_string(data, "name", QT_TO_UTF8(preset.name));
		if (!preset.thumbnail.isEmpty())
			obs_data_set_string(data, "thumbnail", QT_TO_UTF8(preset.thumbnail));
		if (!preset.onCamera()) {
			OBSDataAutoRelease values = obs_data_create();
			obs_data_apply(values, preset.values);
			obs_data_set_obj(data, "values", values);
		}
		/* Nothing but an id: the camera is the one to say it has it */
		if (camera.enumerable && dataCount(data) == 1)
			continue;
		obs_data_array_push_back(preset_array, data);
	}
	obs_data_set_array(settings, "presets", preset_array);
	OBSDataArrayAutoRelease order_array = obs_data_array_create();
	for (const QString &id : m_order) {
		OBSDataAutoRelease entry = obs_data_create();
		obs_data_set_string(entry, "id", QT_TO_UTF8(id));
		obs_data_array_push_back(order_array, entry);
	}
	obs_data_set_array(settings, "preset_order", order_array);
}

void PTZDevice::persist() const
{
	OBSSourceAutoRelease filter = filterSource();
	if (!filter)
		return;
	OBSDataAutoRelease settings = obs_source_get_settings(filter);
	persistState(settings);
	saveDefaults(settings);
}

void PTZDevice::publish() const
{
	OBSSourceAutoRelease filter = filterSource();
	if (!filter)
		return;
	OBSDataAutoRelease settings = obs_source_get_settings(filter);
	saveDefaults(settings);
}

obs_properties_t *PTZDevice::get_obs_properties()
{
	obs_properties_t *rtn_props = obs_properties_create();

	obs_properties_t *config = obs_properties_create();
	obs_properties_add_group(rtn_props, "interface", obs_module_text("PTZ.Device.Connection"), OBS_GROUP_NORMAL,
				 config);

	/* Generic camera limits and speeds properties */
	auto speed = obs_properties_create();
	obs_properties_add_group(rtn_props, "general", obs_module_text("PTZ.Device.CameraSettings"), OBS_GROUP_NORMAL,
				 speed);
	/* How many slots to use of a camera that can't say how many it has */
	if (slotPresets() && cameraPresets().available)
		obs_properties_add_int_slider(speed, "preset_max", obs_module_text("PTZ.Device.MaxPresets"), 1, 0x80,
					      1);
	for (const AxisInfo &info : kAxes) {
		obs_properties_add_float_slider(speed, settingKey(info.name, "speed_max").c_str(),
						obs_module_text(info.speedMaxText), 0.1, 1.0, 1.0 / 1024);
		obs_properties_add_bool(speed, settingKey(info.name, "invert").c_str(),
					obs_module_text(info.invertText));
	}
	if (features().testFlag(TallyLight))
		obs_properties_add_bool(speed, "tally_auto", obs_module_text("PTZ.Device.TallyAuto"));

	return rtn_props;
}

void ptz_filter_defaults_mark(obs_data_t *defaults)
{
	obs_data_set_default_int(defaults, PTZ_DEFAULTS_MAJOR_KEY, PTZ_API_VERSION_MAJOR);
	obs_data_set_default_int(defaults, PTZ_DEFAULTS_MINOR_KEY, PTZ_API_VERSION_MINOR);
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
	PTZDevice::upgradeSettings(settings);
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
		/* So that there is state to give to another thread from the first */
		if (ptz)
			ptz->refreshStateSnapshot();
	};
	/* Creating the device must happen on the main thread */
	if (QThread::currentThread() != qApp->thread())
		QMetaObject::invokeMethod(qApp, build, Qt::BlockingQueuedConnection);
	else
		build();
	return ptz;
}

/* Without a video_render, libobs draws the filter's target with no effect, and
 * an image, which leaves the effect to its caller, draws nothing */
void ptz_filter_video_render(void *data, gs_effect_t *)
{
	OBSSourceAutoRelease filter = static_cast<PTZDevice *>(data)->filterSource();
	/* OBS takes the parent of a filter that may not be duplicated away when
	 * it frees a copy of the source: nothing to skip to, as in softCamRender */
	if (filter && obs_filter_get_parent(filter) && obs_filter_get_target(filter))
		obs_source_skip_video_filter(filter);
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
	if (ptz)
		ptz->setParentSource(nullptr);
}

void ptz_filter_destroy(void *data)
{
	auto ptz = static_cast<PTZDevice *>(data);
	if (ptz)
		ptz->deleteLater();
}

void ptz_filter_save(void *data, obs_data_t *settings)
{
	auto ptz = static_cast<PTZDevice *>(data);
	if (!ptz)
		return;
	/* The settings are the one copy of what the device saves, which it
	 * keeps current as it changes it. Writing the rest from the device here
	 * would put the device's values over a change made to the settings that
	 * it has yet to be updated with. */
	ptz->persistState(settings);
	/* The filter already knows its source, and a device id isn't stable
	 * across a driver change; neither belongs in the scene collection.
	 * Also clears them from collections saved before this was stripped. */
	PTZDevice::stripIdentity(settings);
	/* Defaults, from ptz_filter_defaults_mark() and the kind, not settings */
	obs_data_erase(settings, PTZ_DEFAULTS_MAJOR_KEY);
	obs_data_erase(settings, PTZ_DEFAULTS_MINOR_KEY);
}

/* One frontend callback for the module's devices, and the lifecycle and
 * device list it drives (main thread only, see PTZLifecycle) */
void ptz_module_frontend_event(enum obs_frontend_event event, void *)
{
	switch (event) {
	case OBS_FRONTEND_EVENT_FINISHED_LOADING:
		module_lifecycle().loadingFinished();
		break;
	case OBS_FRONTEND_EVENT_SCENE_CHANGED:
	case OBS_FRONTEND_EVENT_PREVIEW_SCENE_CHANGED:
	case OBS_FRONTEND_EVENT_STUDIO_MODE_ENABLED:
	case OBS_FRONTEND_EVENT_STUDIO_MODE_DISABLED:
		/* A copy: nothing here destroys one, but be safe */
		for (PTZDevice *d : std::vector<PTZDevice *>(module_devices()))
			d->onSceneChanged();
		break;
	case OBS_FRONTEND_EVENT_SCRIPTING_SHUTDOWN:
		module_lifecycle().exiting();
		break;
	default:
		break;
	}
}

PTZDevice::CameraPresets PTZDevice::cameraPresets() const
{
	CameraPresets camera;
	if (features() & Presets)
		camera.available = true;
	return camera;
}

/* A camera with numbered slots: the first that the list has no preset in, or
 * none if the "preset_max" slots are all in use, counting those that requests in flight have
 * taken. It is a command with no answer, so it is done at once */
bool PTZDevice::cameraPresetCreate(const QString &request, const QString &name)
{
	Q_UNUSED(name);
	if (!slotPresets())
		return false;
	QStringList taken = m_creatingSlot.values();
	for (int slot = 0; slot < (int)m_maxPresets; slot++) {
		if (presetIndex(presetId(QStringLiteral("camera"), QString::number(slot))) >= 0 ||
		    taken.contains(QString::number(slot)))
			continue;
		m_creatingSlot.insert(request, QString::number(slot));
		memory_set(slot);
		QMetaObject::invokeMethod(
			this, [this, request, slot] { cameraPresetCreated(request, QString::number(slot)); },
			Qt::QueuedConnection);
		return true;
	}
	return false;
}

void PTZDevice::cameraPresetSave(const QString &key)
{
	memory_set(key.toInt());
}

void PTZDevice::cameraPresetRecall(const QString &key)
{
	memory_recall(key.toInt());
}

void PTZDevice::cameraPresetDelete(const QString &key)
{
	memory_reset(key.toInt());
}

int PTZDevice::presetIndex(const QString &id) const
{
	for (int i = 0; i < m_presets.size(); i++)
		if (m_presets.at(i).id == id)
			return i;
	return -1;
}

/* The list from what was saved, then what the camera said it has: the camera's
 * presets that were not saved are added, and with a camera that can say, the
 * saved ones it doesn't have are not kept */
void PTZDevice::loadPresets(obs_data_array_t *saved, obs_data_array_t *order)
{
	CameraPresets camera = cameraPresets();
	QList<Preset> loaded;
	for (size_t i = 0; saved && i < obs_data_array_count(saved); i++) {
		OBSDataAutoRelease item = obs_data_array_item(saved, i);
		if (!obs_data_has_user_value(item, "id"))
			continue;
		Preset preset;
		preset.id = QT_UTF8(obs_data_get_string(item, "id"));
		if (preset.id.isEmpty()) {
			/* Older versions kept an int id, which is the camera's slot, or
			 * a token of the camera's own, where it had one */
			QString token = QT_UTF8(obs_data_get_string(item, "token"));
			preset.id = presetId(QStringLiteral("camera"),
					     token.isEmpty() ? QString::number(obs_data_get_int(item, "id")) : token);
		}
		bool seen = false;
		for (const Preset &other : loaded)
			seen = seen || other.id == preset.id;
		if (seen)
			continue;
		preset.name = QT_UTF8(obs_data_get_string(item, "name"));
		preset.thumbnail = QT_UTF8(obs_data_get_string(item, "thumbnail"));
		if (!preset.onCamera()) {
			OBSDataAutoRelease values = obs_data_get_obj(item, "values");
			preset.values = obs_data_create();
			obs_data_release(preset.values);
			if (values)
				obs_data_apply(preset.values, values);
		}
		loaded.append(preset);
	}

	if (m_cameraPresetsKnown) {
		QSet<QString> have;
		for (const auto &[key, name] : m_cameraPresets) {
			QString id = presetId(QStringLiteral("camera"), key);
			have.insert(id);
			bool found = false;
			for (Preset &preset : loaded) {
				if (preset.id != id)
					continue;
				found = true;
				preset.cameraName = name;
				if (camera.namesOnCamera)
					preset.name = name;
			}
			if (!found) {
				Preset preset;
				preset.id = id;
				preset.name = preset.cameraName = name;
				loaded.append(preset);
			}
		}
		for (int i = loaded.size() - 1; i >= 0; i--)
			if (loaded.at(i).onCamera() && !have.contains(loaded.at(i).id))
				loaded.removeAt(i);
	}
	m_presets = loaded;
	/* The order that was saved, or if none was the order the presets were */
	m_order.clear();
	for (size_t i = 0; order && i < obs_data_array_count(order); i++) {
		OBSDataAutoRelease entry = obs_data_array_item(order, i);
		m_order << QT_UTF8(obs_data_get_string(entry, "id"));
	}
	reconcileOrder();
}

/* The order has each preset once: those it has not got go last, in the order they
 * are in. What it has that is not a preset is dropped, once there is a way to
 * know it will not be one: a camera that cannot say what it has does not have it,
 * and one that can has said */
void PTZDevice::reconcileOrder()
{
	bool prune = !cameraPresets().enumerable || m_cameraPresetsKnown;
	QStringList result;
	for (const QString &id : m_order)
		if (!result.contains(id) && (presetIndex(id) >= 0 || !prune))
			result << id;
	for (const Preset &preset : m_presets)
		if (!result.contains(preset.id))
			result << preset.id;
	m_order = result;
}

void PTZDevice::signalOrderChanged()
{
	calldata_t cd = {};
	signalDevice("ptz_preset_order_changed", &cd);
	calldata_free(&cd);
}

void PTZDevice::setCameraPresets(const QList<QPair<QString, QString>> &presets)
{
	QList<Preset> before = m_presets;
	m_cameraPresets = presets;
	m_cameraPresetsKnown = true;

	/* The saved ones are what is here already */
	OBSDataAutoRelease persisted = obs_data_create();
	persistState(persisted);
	OBSDataArrayAutoRelease saved = obs_data_get_array(persisted, "presets");
	OBSDataArrayAutoRelease order = obs_data_get_array(persisted, "preset_order");
	loadPresets(saved, order);

	bool changed = before.size() != m_presets.size();
	for (int i = 0; !changed && i < before.size(); i++)
		changed = before.at(i).id != m_presets.at(i).id || before.at(i).name != m_presets.at(i).name ||
			  before.at(i).cameraName != m_presets.at(i).cameraName;
	if (!changed)
		return;
	for (const Preset &old : before)
		if (presetIndex(old.id) < 0)
			ptz_thumbnail_remove(old.thumbnail);
	persist();
	calldata_t cd = {};
	signalDevice("ptz_preset_list_reset", &cd);
	calldata_free(&cd);
}

void PTZDevice::signalPreset(const char *name, const QString &id)
{
	calldata_t cd = {};
	calldata_set_string(&cd, "id", QT_TO_UTF8(id));
	signalDevice(name, &cd);
	calldata_free(&cd);
}

void PTZDevice::signalPresetChanged(const QString &id, obs_data_t *changed)
{
	calldata_t cd = {};
	calldata_set_string(&cd, "id", QT_TO_UTF8(id));
	calldata_set_ptr(&cd, "changed", changed);
	signalDevice("ptz_preset_changed", &cd);
	calldata_free(&cd);
}

void PTZDevice::signalPresetThumbnail(const QString &id)
{
	int index = presetIndex(id);
	if (index < 0)
		return;
	OBSDataAutoRelease changed = obs_data_create();
	obs_data_set_string(changed, "thumbnail", QT_TO_UTF8(ptz_thumbnail_path(m_presets.at(index).thumbnail)));
	signalPresetChanged(id, changed);
}

/* Replace the preset's thumbnail image, removing the old file. Each image
 * gets a fresh file name so nothing caching the old one can go stale, which
 * names the device's filter (see ptz_thumbnail_new_name()). */
void PTZDevice::setPresetThumbnail(const QString &id, const QImage &image)
{
	int index = presetIndex(id);
	if (index < 0)
		return;
	OBSSourceAutoRelease filter = filterSource();
	QString name = ptz_thumbnail_write(image, filter ? QString::fromUtf8(obs_source_get_uuid(filter)) : QString());
	if (name.isEmpty())
		return;
	ptz_thumbnail_remove(m_presets[index].thumbnail);
	m_presets[index].thumbnail = name;
	persist();
	signalPresetThumbnail(id);
}

void PTZDevice::clearPresetThumbnail(const QString &id)
{
	int index = presetIndex(id);
	if (index < 0 || m_presets.at(index).thumbnail.isEmpty())
		return;
	ptz_thumbnail_remove(m_presets.at(index).thumbnail);
	m_presets[index].thumbnail.clear();
	persist();
	signalPresetThumbnail(id);
}

void PTZDevice::capturePresetThumbnail(const QString &id)
{
	if (presetIndex(id) < 0)
		return;
	OBSSourceAutoRelease src = parentSource();
	if (!src)
		return;
	ptz_capture_source_thumbnail(src, this, [this, id](QImage image) { setPresetThumbnail(id, image); });
}

void PTZDevice::cancelRecall()
{
	++m_recallGeneration;
	m_recallPending.clear();
	m_arrivedId.clear();
}

void PTZDevice::finishRecall()
{
	QString id = m_recallPending;
	m_recallPending.clear();
	m_arrivedId = id;
	signalPreset("ptz_preset_recalled", id);
}

void PTZDevice::moveFinished()
{
	if (!m_recallPending.isEmpty())
		finishRecall();
}

bool PTZDevice::localPresets() const
{
	return features() & (PanAbs | TiltAbs | ZoomAbs | FocusAbs);
}

QStringList PTZDevice::valueKeys() const
{
	QStringList keys;
	Features f = features();
	if (f & PanAbs)
		keys << QStringLiteral("pan");
	if (f & TiltAbs)
		keys << QStringLiteral("tilt");
	if (f & ZoomAbs)
		keys << QStringLiteral("zoom");
	/* Autofocus on or off, if not a position */
	if (f & (FocusAbs | AutoFocus))
		keys << QStringLiteral("focus");
	return keys;
}

/* Only what the device has reported: it can't be asked, only read from what it
 * last said */
OBSData PTZDevice::captureValues() const
{
	OBSDataAutoRelease values = obs_data_create();
	QStringList keys = valueKeys();
	for (const char *axis : {"pan", "tilt", "zoom"})
		if (keys.contains(QString::fromUtf8(axis)) && obs_data_has_user_value(state, axis))
			obs_data_set_double(values, axis, obs_data_get_double(state, axis));
	if (keys.contains(QStringLiteral("focus"))) {
		OBSDataAutoRelease focus = obs_data_create();
		Features f = features();
		bool any = false;
		if ((f & FocusAbs) && obs_data_has_user_value(state, "focus")) {
			obs_data_set_double(focus, "position", obs_data_get_double(state, "focus"));
			any = true;
		}
		if ((f & AutoFocus) && obs_data_has_user_value(state, "focus_af_enabled")) {
			obs_data_set_bool(focus, "af_enabled", obs_data_get_bool(state, "focus_af_enabled"));
			any = true;
		}
		if (any)
			obs_data_set_obj(values, "focus", focus);
	}
	return values.Get();
}

/* Go where the preset says, for the pan, tilt, zoom and focus it has: the pan
 * and tilt of a camera go together, so one that it has alone is with the other
 * as it is now */
void PTZDevice::applyValues(const Preset &preset)
{
	auto restores = [&](const char *key) {
		return obs_data_has_user_value(preset.values, key);
	};
	Features f = features();
	OBSDataAutoRelease positions = obs_data_create();
	if (restores("pan") && (f & PanAbs))
		obs_data_set_double(positions, "pan", obs_data_get_double(preset.values, "pan"));
	if (restores("tilt") && (f & TiltAbs))
		obs_data_set_double(positions, "tilt", obs_data_get_double(preset.values, "tilt"));
	pairPanTilt(positions.Get());
	if (restores("zoom") && (f & ZoomAbs))
		obs_data_set_double(positions, "zoom", obs_data_get_double(preset.values, "zoom"));
	if (restores("focus") && (f & (FocusAbs | AutoFocus))) {
		OBSDataAutoRelease focus = obs_data_get_obj(preset.values, "focus");
		bool autofocus = obs_data_has_user_value(focus, "af_enabled") && obs_data_get_bool(focus, "af_enabled");
		if ((f & AutoFocus) && obs_data_has_user_value(focus, "af_enabled"))
			set_autofocus(autofocus);
		if (!autofocus && (f & FocusAbs) && obs_data_has_user_value(focus, "position"))
			obs_data_set_double(positions, "focus", obs_data_get_double(focus, "position"));
	}
	if (dataCount(positions))
		moveAbs(positions.Get());
}

/* A camera goes to a pan and tilt together, so one of them alone in
 * `positions` is with the other where it is now, or neither is there, if that
 * isn't known */
void PTZDevice::pairPanTilt(obs_data_t *positions)
{
	bool pan = obs_data_has_user_value(positions, "pan"), tilt = obs_data_has_user_value(positions, "tilt");
	if (pan == tilt)
		return;
	const char *other = pan ? "tilt" : "pan";
	if (obs_data_has_user_value(state, other)) {
		obs_data_set_double(positions, other, obs_data_get_double(state, other));
	} else {
		ptz_info("not moving %s alone: where the camera is not known", pan ? "pan" : "tilt");
		obs_data_erase(positions, pan ? "pan" : "tilt");
	}
}

/* Not the same for two devices, since the listeners of several may be told apart by it alone */
QString PTZDevice::newCreateRequest()
{
	return QStringLiteral("create-") + QUuid::createUuid().toString(QUuid::Id128).left(8);
}

/* The result of a request to make a preset is a signal, which comes after the proc
 * that asked has returned, so that its caller has the request to know it by */
void PTZDevice::finishCreate(const QString &request, const QString &id)
{
	m_creating.remove(request);
	m_creatingSlot.remove(request);
	calldata_t cd = {};
	calldata_set_string(&cd, "request", QT_TO_UTF8(request));
	calldata_set_string(&cd, "id", QT_TO_UTF8(id));
	signalDevice("ptz_preset_create_done", &cd);
	calldata_free(&cd);
}

QString PTZDevice::createPreset(const QString &name, const QString &store)
{
	if (store == QStringLiteral("local")) {
		if (!localPresets())
			return QString();
		QString request = newCreateRequest();
		Preset preset;
		preset.id = presetId(QStringLiteral("local"), QUuid::createUuid().toString(QUuid::Id128).left(8));
		preset.name = name;
		preset.values = captureValues();
		m_presets.append(preset);
		m_order << preset.id;
		persist();
		signalPreset("ptz_preset_added", preset.id);
		capturePresetThumbnail(preset.id);
		m_creating.insert(request, name);
		QMetaObject::invokeMethod(
			this, [this, request, id = preset.id] { finishCreate(request, id); }, Qt::QueuedConnection);
		return request;
	}
	if (store != QStringLiteral("camera"))
		return QString();
	if (!cameraPresets().available)
		return QString();
	QString request = newCreateRequest();
	m_creating.insert(request, name);
	if (!cameraPresetCreate(request, name)) {
		m_creating.remove(request);
		return QString();
	}
	return request;
}

/* What a driver says when the camera has made the preset, or has not */
void PTZDevice::cameraPresetCreated(const QString &request, const QString &key)
{
	if (!m_creating.contains(request))
		return;
	if (key.isEmpty()) {
		finishCreate(request, QString());
		return;
	}
	QString name = m_creating.value(request);
	CameraPresets camera = cameraPresets();
	QString id = presetId(QStringLiteral("camera"), key);
	int index = presetIndex(id);
	if (index < 0) {
		Preset preset;
		preset.id = id;
		preset.name = name;
		if (camera.namesOnCamera)
			preset.cameraName = name;
		m_presets.append(preset);
		m_order << id;
		index = m_presets.size() - 1;
		persist();
		signalPreset("ptz_preset_added", id);
	}
	/* What it has said it has includes this: or the next update() drops it */
	if (camera.enumerable) {
		bool listed = false;
		for (const auto &entry : m_cameraPresets)
			listed = listed || entry.first == key;
		if (!listed)
			m_cameraPresets.append({key, name});
	}
	capturePresetThumbnail(id);
	finishCreate(request, id);
}

void PTZDevice::savePreset(const QString &id)
{
	if (id.startsWith(QStringLiteral("local:"))) {
		int index = presetIndex(id);
		if (index < 0)
			return;
		m_presets[index].values = captureValues();
		persist();
		OBSDataAutoRelease changed = obs_data_create();
		OBSDataAutoRelease values = obs_data_create();
		obs_data_apply(values, m_presets.at(index).values);
		obs_data_set_obj(changed, "values", values);
		signalPresetChanged(id, changed);
		capturePresetThumbnail(id);
		return;
	}
	if (!id.startsWith(QStringLiteral("camera:")))
		return;
	cameraPresetSave(presetKey(id));
	/* A slot that the list didn't have, on a camera that can't say what it has */
	CameraPresets camera = cameraPresets();
	if (presetIndex(id) < 0 && camera.available && !camera.enumerable) {
		Preset preset;
		preset.id = id;
		m_presets.append(preset);
		m_order << id;
		persist();
		signalPreset("ptz_preset_added", id);
	}
	capturePresetThumbnail(id);
}

void PTZDevice::recallPreset(const QString &id)
{
	bool local = id.startsWith(QStringLiteral("local:"));
	if (local ? presetIndex(id) < 0 : !id.startsWith(QStringLiteral("camera:")))
		return;
	/* Pending until the driver says the camera is there, or until a timer:
	 * the time to get there if it can't tell, a backstop if it can. Recorded
	 * first, in case a driver knows at once. */
	cancelRecall();
	m_recallPending = id;
	if (local)
		applyValues(m_presets.at(presetIndex(id)));
	else
		cameraPresetRecall(presetKey(id));
	int wait = features().testFlag(MoveCompletion) ? kRecallBackstopMs : kRecallSettleMs;
	uint generation = m_recallGeneration;
	QTimer::singleShot(wait, Qt::PreciseTimer, this, [this, generation] {
		if (generation == m_recallGeneration && !m_recallPending.isEmpty())
			finishRecall();
	});
}

void PTZDevice::deletePreset(const QString &id)
{
	if (id.startsWith(QStringLiteral("camera:"))) {
		cameraPresetDelete(presetKey(id));
		for (int i = m_cameraPresets.size() - 1; i >= 0; i--)
			if (m_cameraPresets.at(i).first == presetKey(id))
				m_cameraPresets.removeAt(i);
	}
	int index = presetIndex(id);
	if (index < 0)
		return;
	ptz_thumbnail_remove(m_presets.at(index).thumbnail);
	m_presets.removeAt(index);
	m_order.removeAll(id);
	persist();
	signalPreset("ptz_preset_removed", id);
}

void PTZDevice::updatePreset(const QString &id, obs_data_t *changes)
{
	int index = presetIndex(id);
	if (index < 0)
		return;
	OBSDataAutoRelease changed = obs_data_create();
	if (obs_data_has_user_value(changes, "name")) {
		QString name = QT_UTF8(obs_data_get_string(changes, "name"));
		/* On a camera that keeps names it is the camera's to say */
		if (m_presets.at(index).onCamera() && cameraPresets().namesOnCamera) {
			cameraPresetRename(presetKey(id), name);
			m_presets[index].cameraName = name;
			for (auto &entry : m_cameraPresets)
				if (entry.first == presetKey(id))
					entry.second = name;
		}
		m_presets[index].name = name;
		obs_data_set_string(changed, "name", QT_TO_UTF8(name));
	}
	if (!m_presets.at(index).onCamera()) {
		OBSDataAutoRelease values = obs_data_get_obj(changes, "values");
		if (values) {
			m_presets[index].values = obs_data_create();
			obs_data_release(m_presets[index].values);
			obs_data_apply(m_presets[index].values, values);
			obs_data_set_obj(changed, "values", values);
		}
	}
	if (obs_data_has_user_value(changes, "thumbnail")) {
		QString path = QT_UTF8(obs_data_get_string(changes, "thumbnail"));
		if (path.isEmpty()) {
			clearPresetThumbnail(id);
		} else {
			QImage image(path);
			if (!image.isNull())
				setPresetThumbnail(id, image);
		}
	}
	persist();
	if (dataCount(changed))
		signalPresetChanged(id, changed);
}

/* `index` is where it goes in the order as it is once the preset is taken out of
 * its old place */
void PTZDevice::movePreset(const QString &id, int index)
{
	reconcileOrder();
	if (presetIndex(id) < 0 || !m_order.contains(id))
		return;
	/* What is shown: the order of the presets there are */
	QStringList shown;
	for (const QString &other : m_order)
		if (presetIndex(other) >= 0)
			shown << other;
	int from = shown.indexOf(id);
	int to = std::clamp(index, 0, (int)shown.size() - 1);
	if (to == from)
		return;
	shown.removeAt(from);
	/* Before the one that is there now, or last */
	QString before = to < shown.size() ? shown.at(to) : QString();
	m_order.removeAll(id);
	m_order.insert(before.isEmpty() ? m_order.size() : m_order.indexOf(before), id);
	persist();
	signalOrderChanged();
}

void PTZDevice::incrementStatistic(const char *name, int amount)
{
	obs_data_set_int(statistics, name, obs_data_get_int(statistics, name) + amount);
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

/**
 * Fires one of sigs' own signals, saying which device it is with its filter.
 * First grab a strong reference to the filter to guarantee the signal handler
 * is valid. Otherwise the filter could be destroyed in parallel, risking a
 * use-after-free. */
void PTZDevice::signalDevice(const char *name, calldata_t *cd)
{
	OBSSourceAutoRelease filter = obs_weak_source_get_source(m_filter);
	if (!filter)
		return;
	/* Lent to the listeners for the call: one that wants it later takes its own reference */
	calldata_set_ptr(cd, "source", filter.Get());
	signal_handler_signal(sigs, name, cd);
}

void PTZDevice::notifyStateChanged()
{
	/* Nothing changed is nothing to say, as a camera that is polled gives */
	obs_data_item_t *first = obs_data_first(stateChanged);
	if (!first)
		return;
	obs_data_item_release(&first);
	/* Before it is said, so that a listener that reads on hearing it gets what
	 * it said, or something newer, from any thread */
	refreshStateSnapshot();
	calldata_t cd = {};
	calldata_set_ptr(&cd, "changed", stateChanged);
	signalDevice("ptz_state_changed", &cd);
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
