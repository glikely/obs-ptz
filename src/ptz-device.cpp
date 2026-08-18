/* Pan Tilt Zoom Controls - PTZDevice base object
 *
 * Copyright 2020-2026 Grant Likely <grant.likely@secretlab.ca>
 *
 * SPDX-License-Identifier: GPLv2
 */

#include <obs.hpp>
#include <algorithm>
#include <QHash>
#include "ptz-device.hpp"
#include "ptz-list-model.hpp"
#include "ptz-visca-udp.hpp"
#include "ptz-visca-tcp.hpp"
#include "ptz-onvif.hpp"
#include "ptz-usb-cam.hpp"
#include "ptz.h"
#include "protocol-helpers.hpp"

#if defined(ENABLE_SERIALPORT)
#include "ptz-visca-uart.hpp"
#include "ptz-pelco.hpp"
#endif

/**
 * The only place PTZDevice* pointers live outside the object itself. Used
 * for unique-id assignment and the handful of C-linkage entry points
 * (ptz_devices_get_config(), ptz_device_find_source_using_ptz_name(), the
 * legacy scripting proc_handler) that need to reach a specific device
 * directly. PTZListModel never sees this -- it only ever gets a device_id
 * plus the proc_handler_t / signal_handler_t pointers handed to it over the
 * global signal_handler in ptz_device_create()/~PTZDevice().
 */
static QHash<uint32_t, PTZDevice *> ptz_device_registry;

static PTZDevice *find_device_by_name(const QString &name)
{
	for (auto ptz : ptz_device_registry)
		if (name == ptz->objectName())
			return ptz;
	return nullptr;
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

PTZDevice::PTZDevice(OBSData config) : QObject()
{
	/* Create and populate the proc handler methods */
	handler = proc_handler_create();
	if (!handler) {
		blog(LOG_ERROR, "could not allocate proc_handler for %s", obs_data_get_string(config, "name"));
		return;
	}

	/* The PTZ Device API. All these functions are prefixed with 'ptz_' so that they can
	 * be added to an existing proc_handler with low risk of conflicts */
	proc_handler_add(handler, "void ptz_stop()", ptz_ph_lambda(stop), this);
	proc_handler_add(handler, "void ptz_home_recall()", ptz_ph_lambda(pantilt_home), this);
	proc_handler_add(handler, "void ptz_home_save()", ptz_ph_lambda(pantilt_set_home), this);
	proc_handler_add(handler, "void ptz_move()", ptz_ph_lambda(move), this);
	proc_handler_add(handler, "void ptz_move_abs()", ptz_ph_lambda(move_abs), this);
	proc_handler_add(handler, "void ptz_move_rel()", ptz_ph_lambda(move_rel), this);
	proc_handler_add(handler, "void ptz_get()", ptz_ph_lambda(get), this);
	proc_handler_add(handler, "void ptz_set()", ptz_ph_lambda(set), this);
	proc_handler_add(handler, "void ptz_preset_save()", ptz_ph_lambda(preset_save), this);
	proc_handler_add(handler, "void ptz_preset_recall()", ptz_ph_lambda(preset_recall), this);
	proc_handler_add(handler, "void ptz_preset_clear()", ptz_ph_lambda(preset_clear), this);

	/* Query/config/preset-CRUD API for PTZListModel -- everything it
	 * needs from a PTZDevice beyond movement/preset-recall control,
	 * without calling PTZDevice methods directly */
	proc_handler_add(handler, "ptr ptz_get_state()", ptz_ph_lambda(get_state), this);
	proc_handler_add(handler, "void ptz_set_name(string name)", ptz_ph_lambda(setObjectName), this);
	proc_handler_add(handler, "void ptz_set_locked(bool locked)", ptz_ph_lambda(setLock), this);
	proc_handler_add(handler, "void ptz_get_config(ptr config)", ptz_ph_lambda(get_config), this);
	proc_handler_add(handler, "void ptz_set_config(ptr config)", ptz_ph_lambda(set_config), this);
	proc_handler_add(handler, "ptr ptz_get_properties()", ptz_ph_lambda(get_obs_properties), this);
	proc_handler_add(handler, "ptr ptz_preset_get_list()", ptz_ph_lambda(preset_get_list), this);
	proc_handler_add(handler, "int ptz_preset_new(int row)", ptz_ph_lambda(newPreset), this);
	proc_handler_add(handler, "void ptz_preset_remove(int row)", ptz_ph_lambda(removePresetAtDisplayRow), this);
	proc_handler_add(handler, "void ptz_preset_move(int src_row, int dest_row)", ptz_ph_lambda(movePreset), this);
	proc_handler_add(handler, "void ptz_preset_set_name(int id, string name)", ptz_ph_lambda(setPresetName), this);
	proc_handler_add(handler, "void ptz_scene_changed()", ptz_ph_lambda(onSceneChanged), this);

	/* A single change notification, broadcast on a per-device signal
	 * handler so listeners never need a direct C++ reference to this
	 * class -- see PTZListModel::deviceCreated()/device_create_cb(). One
	 * signal covers status, rename, and settings changes alike: none of
	 * them carry enough of a payload on their own for a listener to patch
	 * anything selectively, so there's nothing a separate signal per
	 * change kind would let a listener do differently -- just device_id,
	 * to say which device to re-query. */
	sigs = signal_handler_create();
	if (!sigs) {
		blog(LOG_ERROR, "could not allocate signal_handler for %s", obs_data_get_string(config, "name"));
	} else {
		signal_handler_add(sigs, "void state_changed(int device_id)");

		/* Preset list mutations are bracketed by a before/after signal
		 * pair, exactly like the QAbstractItemModel begin.../end...
		 * calls they replace -- signal_handler_signal() dispatches to
		 * connected callbacks synchronously, so PTZListModel's "before"
		 * callback can still call beginInsertRows()/etc. ahead of the
		 * mutation actually happening. */
		signal_handler_add(sigs, "void preset_insert(int device_id, int row)");
		signal_handler_add(sigs, "void preset_inserted(int device_id, int row)");
		signal_handler_add(sigs, "void preset_remove(int device_id, int row)");
		signal_handler_add(sigs, "void preset_removed(int device_id, int row)");
		signal_handler_add(sigs, "bool preset_move(int device_id, int src_row, int dest_row)");
		signal_handler_add(sigs, "void preset_moved(int device_id, int src_row, int dest_row)");
		signal_handler_add(sigs, "void preset_renamed(int device_id, int id)");
	}

	setObjectName(obs_data_get_string(config, "name"));
	type = obs_data_get_string(config, "type");
	settings = obs_data_create();
	obs_data_release(settings);
	statistics = obs_data_create();
	obs_data_release(statistics);
	obs_data_set_obj(settings, "statistics", statistics);
	stale_settings = {"pan_pos", "tilt_pos", "zoom_pos", "focus_pos"};

	/* Assign a unique ID -- this is the one place a device's identity is
	 * decided, so it happens here rather than in whatever happens to be
	 * listening on the create signal that announceCreated() fires. */
	uint32_t new_id = (uint32_t)obs_data_get_int(config, "id");
	while (ptz_device_registry.contains(new_id) || new_id == 0)
		new_id++;
	id = new_id;
	ptz_device_registry[id] = this;
}

/**
 * Fires the "ptz_device_create" signal -- deliberately *not* done from the
 * constructor above. Driver subclasses call update(config) from their own
 * constructor body, which runs after PTZDevice's base constructor
 * completes, so a signal fired there would let PTZListModel seed its cache
 * (via ptz_get_state()/ptz_preset_get_list()) before presets/settings are
 * actually loaded. ptz_device_create() calls this once the full object
 * (base and derived) is constructed instead.
 */
void PTZDevice::announceCreated()
{
	calldata_t cd = {};
	calldata_set_int(&cd, "device_id", id);
	calldata_set_ptr(&cd, "proc_handler", handler);
	calldata_set_ptr(&cd, "signal_handler", sigs);
	signal_handler_signal(ptz_get_signal_handler(), "ptz_device_create", &cd);
	calldata_free(&cd);
}

PTZDevice::~PTZDevice()
{
	calldata_t cd = {};
	calldata_set_int(&cd, "device_id", id);
	signal_handler_signal(ptz_get_signal_handler(), "ptz_device_destroy", &cd);
	calldata_free(&cd);

	ptz_device_registry.remove(id);

	proc_handler_destroy(handler);
	handler = nullptr;
	signal_handler_destroy(sigs);
	sigs = nullptr;
}

void PTZDevice::setObjectName(QString name)
{
	if (name.simplified().isEmpty()) {
		if (objectName().startsWith(obs_module_text("PTZ.Device.DefaultName")))
			return;
		name = obs_module_text("PTZ.Device.DefaultName");
	}
	if (name == objectName())
		return;
	QString new_name = name;
	for (int i = 1;; i++) {
		PTZDevice *ptz = find_device_by_name(new_name);
		if (!ptz)
			break;
		new_name = name + " " + QString::number(i);
	}
	QObject::setObjectName(new_name);

	calldata_t cd = {};
	calldata_set_int(&cd, "device_id", id);
	signal_handler_signal(sigs, "state_changed", &cd);
	calldata_free(&cd);
}

QString PTZDevice::description()
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
	auto source = obs_get_source_by_name(QT_TO_UTF8(objectName()));
	if (source) {
		auto program = obs_frontend_get_current_scene();
		locked = live = ptz_scene_is_source_active(program, source);
		obs_source_release(program);

		if (obs_frontend_preview_program_mode_active()) {
			auto previewScene = obs_frontend_get_current_preview_scene();
			preview = ptz_scene_is_source_active(previewScene, source);
			obs_source_release(previewScene);
		}

		obs_source_release(source);
	}

	/* PTZListModel's cache of live/preview/locked can only be refreshed
	 * by a signal -- unlike the old direct-pointer design, it doesn't get
	 * these for free just by reading this object's fields on every
	 * paint. */
	if (locked != was_locked || live != was_live || preview != was_preview) {
		calldata_t cd = {};
		calldata_set_int(&cd, "device_id", id);
		signal_handler_signal(sigs, "state_changed", &cd);
		calldata_free(&cd);
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

void PTZDevice::get(calldata_t *cd) const
{
	if (QThread::currentThread() != thread()) {
		ptz_log(LOG_ERROR, "PTZDevice::get(calldata) called from wrong thread; ignored");
		return;
	}
	QString arg = calldata_string(cd, "property");
	if (arg == "power_on")
		calldata_set_bool(cd, "power_on", obs_data_get_bool(settings, "power_on"));
	else if (arg == "focus_af_enabled")
		calldata_set_bool(cd, "focus_af_enabled", obs_data_get_bool(settings, "focus_af_enabled"));
	return;
}

void PTZDevice::set(calldata_t *cd)
{
	bool enable;
	if (calldata_get_bool(cd, "focus_af_enabled", &enable))
		QMetaObject::invokeMethod(this, "set_autofocus", Q_ARG(bool, enable));
	bool trigger;
	if (calldata_get_bool(cd, "focus_onetouch_trigger", &trigger) && trigger)
		QMetaObject::invokeMethod(this, &PTZDevice::focus_onetouch);
}

void PTZDevice::preset_save(calldata_t *cd)
{
	long long id;
	if (calldata_get_int(cd, "preset_id", &id))
		QMetaObject::invokeMethod(this, "memory_set", Q_ARG(int, id));
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
	if (calldata_get_int(cd, "preset_id", &id))
		QMetaObject::invokeMethod(this, "memory_reset", Q_ARG(int, id));
}

/**
 * Returns a caller-owned obs_data_t snapshot of everything PTZListModel
 * needs to display a device row without holding a PTZDevice* -- the caller
 * is responsible for obs_data_release()ing it.
 */
void PTZDevice::get_state(calldata_t *cd)
{
	obs_data_t *state = obs_data_create();
	obs_data_set_string(state, "name", QT_TO_UTF8(objectName()));
	obs_data_set_string(state, "description", QT_TO_UTF8(description()));
	obs_data_set_string(state, "type", type.c_str());
	obs_data_set_bool(state, "connected", connected);
	obs_data_set_bool(state, "live", live);
	obs_data_set_bool(state, "preview", preview);
	obs_data_set_bool(state, "locked", locked);
	obs_data_set_bool(state, "supports_set_home", supportsSetHome());
	obs_data_set_int(state, "max_presets", (long long)m_maxPresets);
	calldata_set_ptr(cd, "return", state);
}

void PTZDevice::setObjectName(calldata_t *cd)
{
	setObjectName(QT_UTF8(calldata_string(cd, "name")));
}

void PTZDevice::setLock(calldata_t *cd)
{
	setLock(calldata_bool(cd, "locked"));
}

/**
 * Fills the caller-owned obs_data_t passed in via the "config" calldata
 * field, mirroring save(OBSData) const.
 */
void PTZDevice::get_config(calldata_t *cd) const
{
	auto config = static_cast<obs_data_t *>(calldata_ptr(cd, "config"));
	if (config)
		save(config);
}

void PTZDevice::set_config(calldata_t *cd)
{
	auto config = static_cast<obs_data_t *>(calldata_ptr(cd, "config"));
	if (config)
		update(config);
}

void PTZDevice::get_obs_properties(calldata_t *cd)
{
	calldata_set_ptr(cd, "return", get_obs_properties());
}

/**
 * Returns a caller-owned obs_data_array_t of {id, name, token} entries in
 * display order, the preset-list equivalent of get_state().
 */
void PTZDevice::preset_get_list(calldata_t *cd) const
{
	obs_data_array_t *list = obs_data_array_create();
	for (auto id : m_presetsDisplayOrder) {
		obs_data_t *item = obs_data_create();
		obs_data_set_int(item, "id", id);
		obs_data_set_string(item, "name", QT_TO_UTF8(presetName(id)));
		obs_data_set_string(item, "token", QT_TO_UTF8(presetToken(id)));
		obs_data_array_push_back(list, item);
		obs_data_release(item);
	}
	calldata_set_ptr(cd, "return", list);
}

void PTZDevice::newPreset(calldata_t *cd)
{
	long long row = -1;
	calldata_get_int(cd, "row", &row);
	calldata_set_int(cd, "return", newPreset((int)row));
}

void PTZDevice::removePresetAtDisplayRow(calldata_t *cd)
{
	removePresetAtDisplayRow((int)calldata_int(cd, "row"));
}

void PTZDevice::movePreset(calldata_t *cd)
{
	movePreset((int)calldata_int(cd, "src_row"), (int)calldata_int(cd, "dest_row"));
}

void PTZDevice::setPresetName(calldata_t *cd)
{
	setPresetName((size_t)calldata_int(cd, "id"), QT_UTF8(calldata_string(cd, "name")));
}

void PTZDevice::getDefaults(OBSData config) const
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

void PTZDevice::update(OBSData config)
{
	getDefaults(config);

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

	setObjectName(obs_data_get_string(config, "name"));
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
	obs_data_set_string(config, "name", QT_TO_UTF8(objectName()));
	obs_data_set_int(config, "id", id);
	obs_data_set_string(config, "type", type.c_str());
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

	/* Combo box list for associated OBS source */
	auto src_cb = [](void *data, obs_source_t *src) {
		auto srcnames = static_cast<QStringList *>(data);
		if (obs_source_get_type(src) != OBS_SOURCE_TYPE_SCENE)
			srcnames->append(obs_source_get_name(src));
		return true;
	};
	auto srcs_prop = obs_properties_add_list(rtn_props, "name", obs_module_text("PTZ.Source"), OBS_COMBO_TYPE_LIST,
						 OBS_COMBO_FORMAT_STRING);
	obs_property_list_add_string(srcs_prop, obs_module_text("PTZ.Device.NoSource"), "");
	/* Add current source to top list */
	OBSSourceAutoRelease src = obs_get_source_by_name(QT_TO_UTF8(objectName()));
	if (src)
		obs_property_list_add_string(srcs_prop, QT_TO_UTF8(objectName()), QT_TO_UTF8(objectName()));
	/* Add all sources not assigned to a camera */
	QStringList srcnames;
	obs_enum_sources(src_cb, &srcnames);
	for (auto ptz : ptz_device_registry)
		srcnames.removeAll(ptz->objectName());
	for (auto n : srcnames)
		obs_property_list_add_string(srcs_prop, QT_TO_UTF8(n), QT_TO_UTF8(n));

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
	if (type == "pelco" || type == "pelco-p")
		ptz = new PTZPelco(config);
	if (type == "visca")
		ptz = new PTZViscaSerial(config);
#endif /* ENABLE_SERIALPORT */
	if (type == "visca-over-ip")
		ptz = new PTZViscaOverIP(config);
	if (type == "visca-over-tcp")
		ptz = new PTZViscaOverTCP(config);
#if defined(ENABLE_ONVIF)
	if (type == "onvif")
		ptz = new PTZOnvif(config);
#endif /* ENABLE_ONVIF */
#if defined(ENABLE_USB_CAM)
	if (type == "usb-cam")
		ptz = new PTZUSBCam(config);
#endif /* ENABLE_USB_CAM */

	/* Only announce once the full (base + derived) object is constructed
	 * -- see PTZDevice::announceCreated()'s comment. */
	if (ptz)
		ptz->announceCreated();
}

void ptz_device_destroy(uint32_t device_id)
{
	delete ptz_device_registry.value(device_id, nullptr);
}

/* C interface for non-QT parts of the plugin */
obs_data_array_t *ptz_devices_get_config()
{
	obs_data_array_t *devices = obs_data_array_create();
	for (auto ptz : ptz_device_registry) {
		OBSDataAutoRelease cfg = obs_data_create();
		ptz->save(cfg.Get());
		obs_data_array_push_back(devices, cfg);
	}
	return devices;
}

obs_source_t *ptz_device_find_source_using_ptz_name(uint32_t device_id)
{
	PTZDevice *ptz = ptz_device_registry.value(device_id, nullptr);
	if (!ptz)
		return NULL;
	return obs_get_source_by_name(QT_TO_UTF8(ptz->objectName()));
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

	/* Deprecated pantilt callback for compatibility with existing plugins */
	proc_handler_add(ph, "void ptz_pantilt(int device_id, float pan, float tilt, float zoom, float focus)", ptz_cb,
			 (void *)"ptz_move");
}

void ptz_unload_devices(void)
{
	/* Reverse of construction order */
	PTZListModel::destroy();

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
	signal_handler_signal(sigs, "preset_renamed", &cd);
	calldata_free(&cd);
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

	calldata_t cd = {};
	calldata_set_int(&cd, "device_id", this->id);
	calldata_set_int(&cd, "row", row);
	signal_handler_signal(sigs, "preset_insert", &cd);

	QVariantMap map;
	map["id"] = (uint)id;
	m_presets[id] = map;
	m_presetsDisplayOrder.insert(row, id);

	signal_handler_signal(sigs, "preset_inserted", &cd);
	calldata_free(&cd);

	return id;
}

void PTZDevice::removePresetAtDisplayRow(int row)
{
	calldata_t cd = {};
	calldata_set_int(&cd, "device_id", this->id);
	calldata_set_int(&cd, "row", row);
	signal_handler_signal(sigs, "preset_remove", &cd);

	m_presets.remove(m_presetsDisplayOrder[row]);
	m_presetsDisplayOrder.removeAt(row);

	signal_handler_signal(sigs, "preset_removed", &cd);
	calldata_free(&cd);
}

void PTZDevice::movePreset(int srcRow, int destRow)
{
	calldata_t cd = {};
	calldata_set_int(&cd, "device_id", this->id);
	calldata_set_int(&cd, "src_row", srcRow);
	calldata_set_int(&cd, "dest_row", destRow);
	signal_handler_signal(sigs, "preset_move", &cd);
	bool ok = calldata_bool(&cd, "return");
	if (!ok) {
		calldata_free(&cd);
		return;
	}

	if (srcRow < destRow)
		destRow--;
	m_presetsDisplayOrder.move(srcRow, destRow);

	calldata_set_int(&cd, "dest_row", destRow);
	signal_handler_signal(sigs, "preset_moved", &cd);
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
	bool was_connected = connected;
	connected = _connected;
	if (was_connected != connected) {
		calldata_t cd = {};
		calldata_set_int(&cd, "device_id", id);
		signal_handler_signal(sigs, "state_changed", &cd);
		calldata_free(&cd);
	}
}

void PTZDevice::setLock(bool state)
{
	if (locked == state)
		return;
	locked = state;
	calldata_t cd = {};
	calldata_set_int(&cd, "device_id", id);
	signal_handler_signal(sigs, "state_changed", &cd);
	calldata_free(&cd);
}

void PTZDevice::notifySettingsChanged()
{
	calldata_t cd = {};
	calldata_set_int(&cd, "device_id", id);
	signal_handler_signal(sigs, "state_changed", &cd);
	calldata_free(&cd);
}
