/* Pan Tilt Zoom Controls - PTZDevice base object
 *
 * Copyright 2020-2026 Grant Likely <grant.likely@secretlab.ca>
 *
 * SPDX-License-Identifier: GPLv2
 */

#include <obs.hpp>
#include <algorithm>
#include <ctime>
#include <functional>
#include <QCoreApplication>
#include <QHash>
#include <QJsonDocument>
#include <QMutex>
#include <QThread>
#include <QTimer>
#include <QUrl>
#include <QUuid>
#include "ptz-device.hpp"
#include "ptz-controls.hpp"
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

/* Everything the PTZ API registers goes through PTZDevice::addProc() and
 * addSignal() for a device's own handlers, which log it, as the scope it is
 * registered in ("device-proc" or "device-signal") and its declaration.
 * ptz_registered_api() hands the log back, so that a test can hold
 * docs/ptz-device-api.md to what the plugin really registers rather than to
 * a copy of it. */
static QMutex ptz_api_log_mutex;
static QList<QPair<QString, QString>> ptz_api_log;

static void ptz_api_logged(const char *scope, const char *decl)
{
	QPair<QString, QString> entry(QString::fromUtf8(scope), QString::fromUtf8(decl));
	QMutexLocker locker(&ptz_api_log_mutex);
	/* A device registers its handlers each time one is made */
	if (!ptz_api_log.contains(entry))
		ptz_api_log.append(entry);
}

static void ptz_proc_add(const char *scope, proc_handler_t *handler, const char *decl, proc_handler_proc_t proc,
			 void *data)
{
	ptz_api_logged(scope, decl);
	proc_handler_add(handler, decl, proc, data);
}

static void ptz_signal_add(const char *scope, signal_handler_t *handler, const char *decl)
{
	ptz_api_logged(scope, decl);
	signal_handler_add(handler, decl);
}

void PTZDevice::addProc(const char *decl, proc_handler_proc_t proc, void *data)
{
	ptz_proc_add("device-proc", handler, decl, proc, data);
}

void PTZDevice::addSignal(const char *decl)
{
	ptz_signal_add("device-signal", sigs, decl);
}

QList<QPair<QString, QString>> ptz_registered_api()
{
	QMutexLocker locker(&ptz_api_log_mutex);
	return ptz_api_log;
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

PTZDevice::PTZDevice(OBSData config, obs_source_t *filter) : QObject()
{
	m_filter = OBSGetWeakRef(filter);

	/* proc_handers for calling into the device. Comes from its filter */
	handler = obs_source_get_proc_handler(filter);
	if (!handler) {
		blog(LOG_ERROR, "could not allocate proc_handler for a PTZ device");
		return;
	}

	auto get_api_version = [](void *, calldata_t *cd) {
		calldata_set_int(cd, "major", PTZ_API_VERSION_MAJOR);
		calldata_set_int(cd, "minor", PTZ_API_VERSION_MINOR);
	};
	/* The device's own version: see docs/ptz-device-api.md */
	addProc("void ptz_get_api_version(out int major, out int minor)", get_api_version, nullptr);

	/* The PTZ Device API, which docs/ptz-device-api.md specifies and
	 * tests/obs-integration/test_api_doc.py holds to what is registered
	 * here. All these functions are prefixed with 'ptz_' so that they can
	 * be added to an existing proc_handler with low risk of conflicts */
	addProc("void ptz_stop()", ptz_ph_lambda(stop), this);
	addProc("void ptz_home_recall()", ptz_ph_lambda(pantilt_home), this);
	addProc("void ptz_home_save()", ptz_ph_lambda(pantilt_set_home), this);
	addProc("void ptz_move()", ptz_ph_lambda(move), this);
	addProc("void ptz_move_abs()", ptz_ph_lambda(move_abs), this);
	addProc("void ptz_move_rel()", ptz_ph_lambda(move_rel), this);
	addProc("void ptz_preset_save(string id)", ptz_ph_lambda(preset_save), this);
	addProc("void ptz_preset_recall(string id)", ptz_ph_lambda(preset_recall), this);

	addProc("ptr ptz_get_state(ptr state)", ptz_ph_lambda(get_state), this);
	addProc("ptr ptz_get_statistics(ptr statistics)", ptz_ph_lambda(get_statistics), this);
	addProc("ptr ptz_get_parent_source()", ptz_ph_lambda(get_parent_source), this);

	addProc("void ptz_request_state(ptr state)", ptz_ph_lambda(request_state), this);

	addProc("void ptz_trigger(string name)", ptz_ph_lambda(trigger), this);

	addProc("void ptz_get_camera_report(out string report)", ptz_ph_lambda(get_camera_report), this);

	addProc("ptr ptz_preset_get_list()", ptz_ph_lambda(preset_get_list), this);
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
	case OBS_FRONTEND_EVENT_SCENE_CHANGED:
	case OBS_FRONTEND_EVENT_PREVIEW_SCENE_CHANGED:
	case OBS_FRONTEND_EVENT_STUDIO_MODE_ENABLED:
	case OBS_FRONTEND_EVENT_STUDIO_MODE_DISABLED:
		onSceneChanged();
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

PTZDevice::~PTZDevice()
{
	if (m_frontendCallback)
		obs_frontend_remove_event_callback(frontendEventCallback, this);

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
	obs_data_set_bool(out, "connected", connected);
	obs_data_set_string(out, "source", QT_TO_UTF8(sourceName()));
	saveFeatures(out, features());
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
		{TallyLight, "tally_light"},
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
	if (features & (PanTiltAbs | ZoomAbs | FocusAbs))
		obs_data_set_bool(names, "presets", true);
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

void PTZDevice::preset_create(calldata_t *cd)
{
	if (wrongThread("ptz_preset_create"))
		return;
	const char *name = "";
	const char *store = "";
	calldata_get_string(cd, "name", &name);
	calldata_get_string(cd, "store", &store);
	QString id = createPreset(QString::fromUtf8(name ? name : ""), QString::fromUtf8(store ? store : ""));
	calldata_set_string(cd, "return", QT_TO_UTF8(id));
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
	obs_data_set_default_int(config, "preset_max", 16);
	obs_data_set_default_double(config, "pantilt_speed_max", 1.0);
	obs_data_set_default_double(config, "zoom_speed_max", 1.0);
	obs_data_set_default_double(config, "focus_speed_max", 1.0);
	obs_data_set_default_bool(config, "pan_invert", false);
	obs_data_set_default_bool(config, "tilt_invert", false);
	obs_data_set_default_bool(config, "zoom_invert", false);
	obs_data_set_default_bool(config, "focus_invert", false);
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

void PTZDevice::update(OBSData config)
{
	/* Clamp to the same range enforced by the properties slider; a corrupt
	 * or hand-edited config must not yield an absurd preset count. */
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
	obs_data_set_string(config, "type", type.c_str());
	obs_data_set_double(config, "pantilt_speed_max", pantilt_speed_max);
	obs_data_set_double(config, "zoom_speed_max", zoom_speed_max);
	obs_data_set_double(config, "focus_speed_max", focus_speed_max);
	obs_data_set_bool(config, "pan_invert", pan_invert);
	obs_data_set_bool(config, "tilt_invert", tilt_invert);
	obs_data_set_bool(config, "zoom_invert", zoom_invert);
	obs_data_set_bool(config, "focus_invert", focus_invert);
	obs_data_set_bool(config, "tally_auto", tally_auto);
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
	/* How many presets the camera keeps, which the local store has no limit on */
	if (cameraPresets().available)
		obs_properties_add_int_slider(speed, "preset_max", obs_module_text("PTZ.Device.MaxPresets"), 1, 0x80,
					      1);
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
	if (features().testFlag(TallyLight))
		obs_properties_add_bool(speed, "tally_auto", obs_module_text("PTZ.Device.TallyAuto"));

	return rtn_props;
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
	QString name = sourceName();
	/* Without a source there's nothing to recognise it by */
	if (name.isEmpty())
		return;
	obs_data_set_string(entry, "name", QT_TO_UTF8(name));
	obs_data_erase(entry, "id");
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
	/* The settings are the one copy of what the device saves, which it
	 * keeps current as it changes it. Writing the rest from the device here
	 * would put the device's values over a change made to the settings that
	 * it has yet to be updated with. */
	ptz->persistState(settings);
	/* The filter already knows its source, and a device id isn't stable
	 * across a driver change; neither belongs in the scene collection.
	 * Also clears them from collections saved before this was stripped. */
	PTZDevice::stripIdentity(settings);
}

bool ptz_source_is_device(obs_source_t *source)
{
	proc_handler_t *ph = source ? obs_source_get_proc_handler(source) : nullptr;
	if (!ph)
		return false;
	calldata_t cd = {};
	bool usable = proc_handler_call(ph, "ptz_get_api_version", &cd) &&
		      calldata_int(&cd, "major") == PTZ_API_VERSION_MAJOR &&
		      calldata_int(&cd, "minor") >= PTZ_API_VERSION_MINOR;
	calldata_free(&cd);
	return usable;
}

void ptz_device_startup(obs_source_t *filter)
{
	/* The data of a PTZ Control filter is its device */
	auto ptz = filter ? static_cast<PTZDevice *>(obs_obj_get_data(filter)) : nullptr;
	if (ptz)
		QMetaObject::invokeMethod(ptz, [ptz]() { ptz->onOBSStartup(); }, Qt::QueuedConnection);
}

void ptz_load_devices()
{
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
}

PTZDevice::CameraPresets PTZDevice::cameraPresets() const
{
	CameraPresets camera;
	if (features() & Presets)
		camera.available = true;
	return camera;
}

/* A camera with numbered slots: the first that the list has no preset in */
QString PTZDevice::cameraPresetCreate(const QString &name)
{
	Q_UNUSED(name);
	for (int slot = 0; slot < (int)m_maxPresets; slot++) {
		if (presetIndex(presetId(QStringLiteral("camera"), QString::number(slot))) >= 0)
			continue;
		memory_set(slot);
		return QString::number(slot);
	}
	return QString();
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
 * gets a fresh file name so nothing caching the old one can go stale. */
void PTZDevice::setPresetThumbnail(const QString &id, const QImage &image)
{
	int index = presetIndex(id);
	if (index < 0)
		return;
	QString name = ptz_thumbnail_write(image);
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

/* Once the camera has had time to reach a recalled preset, replace that
 * preset's thumbnail with what it sees now. A newer recall cancels this one,
 * since the camera will have moved on before the capture. */
void PTZDevice::refreshThumbnailAfterRecall(const QString &id)
{
	uint generation = ++m_recallGeneration;
	PTZControls *controls = PTZControls::getInstance();
	if (!controls || !controls->refreshThumbnailOnRecall())
		return;
	static constexpr int recallSettleMs = 3000;
	QTimer::singleShot(recallSettleMs, this, [this, id, generation] {
		if (generation == m_recallGeneration)
			capturePresetThumbnail(id);
	});
}

bool PTZDevice::localPresets() const
{
	return features() & (PanTiltAbs | ZoomAbs | FocusAbs);
}

QStringList PTZDevice::valueKeys() const
{
	QStringList keys;
	Features f = features();
	if (f & PanTiltAbs)
		keys << QStringLiteral("pan") << QStringLiteral("tilt");
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
		if ((f & FocusAbs) && obs_data_has_user_value(state, "focus"))
			obs_data_set_double(focus, "position", obs_data_get_double(state, "focus"));
		if ((f & AutoFocus) && obs_data_has_user_value(state, "focus_af_enabled"))
			obs_data_set_bool(focus, "af_enabled", obs_data_get_bool(state, "focus_af_enabled"));
		bool any = obs_data_first(focus) != nullptr;
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
	bool pan = restores("pan"), tilt = restores("tilt");
	if ((pan || tilt) && (f & PanTiltAbs)) {
		bool known = (pan || obs_data_has_user_value(state, "pan")) &&
			     (tilt || obs_data_has_user_value(state, "tilt"));
		if (known)
			pantilt_abs(obs_data_get_double(pan ? preset.values : state, "pan"),
				    obs_data_get_double(tilt ? preset.values : state, "tilt"));
		else
			ptz_info("not restoring %s alone: where the camera is not known", pan ? "pan" : "tilt");
	}
	if (restores("zoom") && (f & ZoomAbs))
		zoom_abs(obs_data_get_double(preset.values, "zoom"));
	if (restores("focus") && (f & (FocusAbs | AutoFocus))) {
		OBSDataAutoRelease focus = obs_data_get_obj(preset.values, "focus");
		bool autofocus = obs_data_has_user_value(focus, "af_enabled") && obs_data_get_bool(focus, "af_enabled");
		if ((f & AutoFocus) && obs_data_has_user_value(focus, "af_enabled"))
			set_autofocus(autofocus);
		if (!autofocus && (f & FocusAbs) && obs_data_has_user_value(focus, "position"))
			focus_abs(obs_data_get_double(focus, "position"));
	}
}

QString PTZDevice::createPreset(const QString &name, const QString &store)
{
	if (store == QStringLiteral("local")) {
		if (!localPresets())
			return QString();
		Preset preset;
		preset.id = presetId(QStringLiteral("local"), QUuid::createUuid().toString(QUuid::Id128).left(8));
		preset.name = name;
		preset.values = captureValues();
		m_presets.append(preset);
		m_order << preset.id;
		persist();
		signalPreset("ptz_preset_added", preset.id);
		capturePresetThumbnail(preset.id);
		return preset.id;
	}
	if (store != QStringLiteral("camera"))
		return QString();
	CameraPresets camera = cameraPresets();
	if (!camera.available)
		return QString();
	QString key = cameraPresetCreate(name);
	if (key.isEmpty())
		return QString();
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
	return id;
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
	if (id.startsWith(QStringLiteral("local:"))) {
		int index = presetIndex(id);
		if (index < 0)
			return;
		applyValues(m_presets.at(index));
		refreshThumbnailAfterRecall(id);
		return;
	}
	if (!id.startsWith(QStringLiteral("camera:")))
		return;
	cameraPresetRecall(presetKey(id));
	refreshThumbnailAfterRecall(id);
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
