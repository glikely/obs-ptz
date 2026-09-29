/* Pan Tilt Zoom Controls - PTZDevice base object
 *
 * Copyright 2020-2026 Grant Likely <grant.likely@secretlab.ca>
 *
 * SPDX-License-Identifier: GPLv2
 */
#pragma once

#include <functional>
#include <QObject>
#include <QList>
#include <QMap>
#include <QMutex>
#include <QVariantMap>
#include <obs.hpp>
#include <obs-frontend-api.h>
#include <qt-wrappers.hpp>
#include <util/platform.h>
#include "ptz.h"

#define ptz_log(level, format, ...) \
	blog(level, "[%s/%.12s] " format, this->type.c_str(), QT_TO_UTF8(this->m_parentSourceName), ##__VA_ARGS__)
#define ptz_info(format, ...) ptz_log(LOG_INFO, format, ##__VA_ARGS__)
#define ptz_debug(format, ...) ptz_log(LOG_DEBUG, format, ##__VA_ARGS__)
#define ptz_debug_trace(format, ...) \
	if (this->protocol_trace)    \
	ptz_log(LOG_DEBUG, format, ##__VA_ARGS__)

class PTZDevice : public QObject {
	Q_OBJECT

protected:
	uint32_t id = 0;
	std::string type;
	bool connected = false;
	bool locked = false;
	bool live = false;
	bool preview = false;
	double pan_speed = 0;
	double tilt_speed = 0;
	double pantilt_speed_max = 1.0;
	bool pan_invert = false;
	bool tilt_invert = false;
	bool pantilt_changed = false;
	double zoom_speed = 0;
	double zoom_speed_max = 1.0;
	bool zoom_invert = false;
	bool zoom_changed = false;
	double focus_speed = 0;
	double focus_speed_max = 1.0;
	bool focus_invert = false;
	bool focus_changed = false;

private:
	bool m_frontendCallback = false;
	void onFrontendEvent(enum obs_frontend_event event);
	static void frontendEventCallback(enum obs_frontend_event event, void *data);

protected:
	/* The OBS filter instance that owns this device, or empty for self-managed */
	OBSWeakSource m_filter;
	/* The OBS source this device controls. Weak because the device
	 * doesn't own the source and the user can delete it at any time. */
	mutable OBSWeakSource m_parentSource;
	QString m_parentSourceName;
	mutable QMutex m_parentSourceMutex;
	void watchParentSource(const OBSWeakSource &weak, bool watch) const;
	/* Collection of all presets, keyed by unique integer id.
	 * On cameras that use preset numbers, the id is mapped 1:1 with the
	 * preset number.  */
	size_t m_maxPresets = 16;
	QMap<size_t, QVariantMap> m_presets;
	QList<size_t> m_presetsDisplayOrder;
	void sanitizePreset(size_t id);
	void setConnected(bool connected);
	OBSData state;        /* Transient state of the camera. Isn't saved */
	OBSData stateChanged; /* changed state to be sent via the notify signal */
	OBSData statistics;
	QSet<QString> stale_state;
	void incrementStatistic(const char *name);

	/* Each PTZDevice has a proc handler so methods can be called
	 * from other plugins -- the filter's own for a filter-owned device,
	 * a private one for a self-managed device (see the constructor) */
	proc_handler_t *handler = nullptr;
	/* ...and likewise a signal handler so status changes can sent */
	signal_handler_t *sigs = nullptr;
	void signalDevice(const char *name, calldata_t *cd);
	void notifyStateChanged();
	/* Record one position axis, where the camera is, in the state and in
	 * what changed, in the units of the movement API (see the absolute
	 * position commands below): "pan" and "tilt" in [-1.0, 1.0], "zoom" and
	 * "focus" in [0.0, 1.0], clamped. Doesn't notify, so a driver can report
	 * several at once with the notifyStateChanged() it makes anyway. A change
	 * too small to show is not a change; says whether there was one. */
	bool setPosition(const char *axis, double value);
	bool wrongThread(const char *method) const;

public:
	~PTZDevice();
	PTZDevice(OBSData config, obs_source_t *filter = nullptr);
	uint32_t getId() const { return id; }
	/* Fires the create signal PTZListModel discovers new devices through.
	 * Called by ptz_device_create() once the full object (base and
	 * derived) is constructed -- see the comment on the definition. */
	void announceCreated();

	/* Refresh the device's name from its source -- the device's name is
	 * always the name of its source (or the last one it had, while that
	 * source doesn't exist), or the default name if it has never had one */
	void syncName();
	/* Returns a new reference to the device's source (release it with
	 * obs_source_release()), or NULL if it has none. */
	obs_source_t *parentSource() const;
	/* Returns a new reference to the "PTZ Control" filter that owns this
	 * device (release it with obs_source_release()), or NULL if it is
	 * self-managed or its filter is gone. */
	obs_source_t *filterSource() const;
	void setParentSource(obs_source_t *source);
	void setParentSourceByName(const char *name);
	bool isSelfManaged() const { return !m_filter; }
	virtual QString description() const;
	bool isLive() const { return live; }
	bool isPreview() const { return preview; }
	virtual bool supportsSetHome() const { return false; }
	/* Whether ptz_trigger takes the "scan_inquiries" and "replies_to_log"
	 * diagnostics, for working out what a camera supports */
	virtual bool supportsDiagnostics() const { return false; }
	/* Updates live/preview/locked from the frontend's current scenes.
	 * Virtual so a driver that can act on going live or off it (a tally
	 * light, say) can do so around the base implementation. */
	virtual void onSceneChanged();
	/* OBS has finished loading, or is closing and has not yet cleared its
	 * scenes (which destroys the filters that own devices). No-ops here; a
	 * driver that can act on the app itself starting or stopping (turning
	 * the camera's power on or off, say) overrides one or both. A device
	 * hears of both from OBS itself, not through the UI, so it works the
	 * same wherever the device is made. */
	virtual void onOBSStartup() {}
	virtual void onOBSShutdown() {}
	/* Hands one device an OBS frontend event, as OBS does all of them. For
	 * tests, which can't make OBS finish loading again. False if there is no
	 * such device. */
	static bool deliverFrontendEvent(uint32_t device_id, enum obs_frontend_event event);

	size_t maxPresets() const { return m_maxPresets; }
	int presetCount() const { return m_presetsDisplayOrder.size(); }
	int newPreset(int row = -1);
	void removePresetAtDisplayRow(int row);
	void movePreset(int srcRow, int destRow);
	int presetAtDisplayRow(int row) const;
	QString presetName(size_t id) const { return m_presets[id]["name"].toString(); }
	QString presetToken(size_t id) const { return m_presets[id]["token"].toString(); }
	void setPresetName(size_t id, QString name);
	QVariant presetProperty(size_t id, QString key) const;
	bool updatePreset(size_t id, const QVariantMap &map);
	int findPreset(QString key, QVariant value) const;

	/**
	 * do_update() method is to be implemented by each driver as the way
	 * to communicate movement commands to the camera. Since movement
	 * speed is cached in the generic model, the backend driver can
	 * throttle how many commands are actually sent to the device by
	 * arranging to call itself back at a later time
	 */
	virtual void do_update() = 0;

	/** Movement Methods
	 * All movement commands are normalized to floating point ranges of
	 * -1.0 to 1.0, or 0.0 to 1.0, as listed below.
	 *
	 * Drive commands. Starts a continuous move of the camera that
	 * continues until told to stop, or the far limit of the camera is
	 * reached. Values passed in are the speed of movement. 1.0 means full
	 * speed, and 0.0 means stop. (continuous movement)
	 * pantilt: range[-1.0, 1.0], positive for clockwise, move to up/right
	 * zoom: range[-1.0, 1.0], positive for TELE zoom
	 * focus: range[-1.0, 1.0], positive for near-end focus
	 *
	 * Relative position commands. Perform a specific distance relative to
	 * the current position of the camera. 1.0 == full range movement
	 * pantilt: range[-1.0, 1.0], positive for clockwise, move to up/right
	 * zoom: range[-1.0, 1.0], positive for tele zoom
	 * focus: range[-1.0, 1.0], positive for near-end focus
	 *
	 * Absolute position commands. A 1.0 magnitude value means full range
	 * of movement
	 * pantilt: range[-1.0, 1.0], positive for clockwise, move to up/right
	 * zoom: range[0.0, 1.0], 0.0 == wide angle, 1.0 == telephoto
	 * focus: range[0.0, 1.0], 0.0 == far focus, 1.0 == near focus
	 */
protected slots:
	void stop();
	void pantilt(double pan, double tilt);
	virtual void pantilt_rel(double pan, double tilt)
	{
		Q_UNUSED(pan);
		Q_UNUSED(tilt);
	}
	virtual void pantilt_abs(double pan, double tilt)
	{
		Q_UNUSED(pan);
		Q_UNUSED(tilt);
	}
	virtual void pantilt_home() {}
	/**
	 * pantilt_set_home(): record the camera's *current* pan/tilt/zoom as
	 * its new home position (so a later pantilt_home() returns here).
	 * Optional — drivers that don't implement it should leave the default
	 * empty body and return false from supportsSetHome(), so the UI can
	 * hide the action entirely instead of presenting a dead control.
	 */
	virtual void pantilt_set_home() {}
	void zoom(double speed);
	virtual void zoom_abs(double pos) { Q_UNUSED(pos); };
	virtual void set_autofocus(bool enabled) { Q_UNUSED(enabled); };
	void focus(double speed);
	virtual void focus_abs(double pos) { Q_UNUSED(pos); }
	virtual void focus_onetouch() {}
	virtual void memory_set(int i) { Q_UNUSED(i); }
	virtual void memory_recall(int i) { Q_UNUSED(i); }
	virtual void memory_reset(int i) { Q_UNUSED(i); }

	void stop(calldata_t *) { QMetaObject::invokeMethod(this, "stop"); }
	void pantilt_home(calldata_t *) { QMetaObject::invokeMethod(this, "pantilt_home"); }
	void pantilt_set_home(calldata_t *) { QMetaObject::invokeMethod(this, "pantilt_set_home"); }
	void move(calldata_t *cd);
	void move_abs(calldata_t *cd);
	void move_rel(calldata_t *cd);
	/* ptz_trigger: a one-shot action, see runTrigger() */
	void trigger(calldata_t *cd);
	void preset_save(calldata_t *cd);
	void preset_recall(calldata_t *cd);
	void preset_clear(calldata_t *cd);

	/* calldata_t overloads of the query/config/preset-CRUD API below,
	 * registered on the proc_handler so PTZListModel never has to call
	 * these directly -- see PTZDevice::PTZDevice() for registration and
	 * PTZListModel::refreshDeviceState()/refreshPresetList() for callers */
	void get_state(calldata_t *cd) const;
	void setLock(calldata_t *cd);
	void get_config(calldata_t *cd) const;
	void set_config(calldata_t *cd);
	void get_obs_properties(calldata_t *cd);
	void request_state(calldata_t *cd);
	void preset_get_list(calldata_t *cd) const;
	void newPreset(calldata_t *cd);
	void removePresetAtDisplayRow(calldata_t *cd);
	void movePreset(calldata_t *cd);
	void setPresetName(calldata_t *cd);
	void onSceneChanged(calldata_t *cd)
	{
		Q_UNUSED(cd);
		if (wrongThread("ptz_scene_changed"))
			return;
		onSceneChanged();
	}

public:
	bool isLocked() const { return locked; };
	bool isConnected() const { return connected; }
	void setLock(bool state);
	bool pantiltChanged() const { return pantilt_changed; }
	bool zoomChanged() const { return zoom_changed; }
	bool focusChanged() const { return focus_changed; }

	/* Device configuration methods
	 * These match the pattern used by sources in OBS studio with the following methods:
	 * `defaults()`: loads OBSData with default values for the device. Static
	 *     on each driver, and independent of any one device's settings.
	 * `update()`: which informs the device of changes to the configuration.
	 *     Must be handed a complete settings object, defaults included (a
	 *     filter's own settings are, and so is what save() writes), and
	 *     never modifies it or adds anything to it.
	 * `save()`: Make sure device configuration is written to an OBSData.
	 *     Settings only, apart from the identity keys that stripIdentity()
	 *     removes again before the filter persists the result.
	 */
	static void defaults(obs_data_t *defaults);
	virtual void update(OBSData ptz_config);
	virtual void save(OBSData ptz_config) const;

	/* Apply new settings: update(), then announce it with the
	 * "settings_changed" signal. The one place that fires that signal, so
	 * the filter's .update and the dialog's ptz_set_config both go here.
	 * `settings` must be complete, as update() requires. */
	void applySettings(OBSData settings);
	/* Remove the runtime identity keys save() adds ("name", "id",
	 * "is-self-managed") from a settings object destined for persistence
	 * in a filter, which already knows its own source, and whose device id
	 * isn't stable across a driver change. */
	static void stripIdentity(obs_data_t *settings);

	/* Properties describe how to display the settings in a GUI dialog */
	virtual obs_properties_t *get_obs_properties();

	/* Transient state: what the camera and the host currently report,
	 * never persisted. The parallel of the settings above:
	 * `saveState()`: fills an OBSData with the device's whole current
	 *     state -- name, connection, live/preview/locked, whatever the
	 *     driver has read back from the camera, and "statistics".
	 * `requestState()`: asks for some state to change. Only the keys
	 *     present are acted on, each by issuing the command that should
	 *     make the camera report it; the value isn't stored here, it
	 *     becomes state when the camera says so (or, where a driver
	 *     already does so, optimistically when the command is sent).
	 *     Commandable keys: focus_af_enabled, and per driver power_on,
	 *     wb_mode, tally_on, tally_preview. Anything else is ignored, so a caller can
	 *     hand back state it read, read-only parts and all.
	 * Unlike the settings there is no properties tree for it: it isn't
	 * bound to anything OBS persists, and changes many times a second.
	 */
	virtual void saveState(OBSData state) const;
	virtual void requestState(OBSData requested);

	/* One-shot actions on the camera, which aren't state and so have no
	 * place in requestState(), by name: "focus_onetouch" here, and per
	 * driver "wb_onepush" and the diagnostics. Run on the device's own
	 * thread; says whether the device knows the name. */
	virtual bool runTrigger(const QString &name);
};

/* backend driver hooks that register themselves as an OBS filters */
void *ptz_filter_create(const std::function<PTZDevice *()> &make);
obs_properties_t *ptz_filter_get_properties(void *data);
void ptz_filter_update(void *data, obs_data_t *settings);
void ptz_filter_add(void *data, obs_source_t *parent);
void ptz_filter_remove(void *data, obs_source_t *);
void ptz_filter_destroy(void *data);
void ptz_filter_save(void *data, obs_data_t *settings);
