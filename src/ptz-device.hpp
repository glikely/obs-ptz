/* Pan Tilt Zoom Controls - PTZDevice base object
 *
 * Copyright 2020-2026 Grant Likely <grant.likely@secretlab.ca>
 *
 * SPDX-License-Identifier: GPLv2
 */
#pragma once

#include <functional>
#include <QImage>
#include <QJsonObject>
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
#ifdef ENABLE_PROTOCOL_TRACE
#define ptz_debug_trace(format, ...) \
	if (this->protocol_trace)    \
	ptz_log(LOG_DEBUG, format, ##__VA_ARGS__)
#else
#define ptz_debug_trace(format, ...)
#endif

/* What the driver's camera does with presets, see PTZDevice::cameraPresets() */
struct PTZCameraPresets {
	/* The camera has presets to use */
	bool available = false;
	/* It keeps the preset's name, and is told when it changes */
	bool namesOnCamera = false;
	/* It can say what presets it has: see PTZDevice::setCameraPresets() */
	bool enumerable = false;
};

class PTZDevice : public QObject {
	Q_OBJECT

public:
	/* What a device can do, so that what controls it offers only that. In
	 * its state as "features": an object with each one's name (see
	 * featureNames()) true. */
	enum Feature : uint32_t {
		/* Moving at a speed: pantilt(), zoom() and focus() */
		PanTilt = 1 << 0,
		Zoom = 1 << 1,
		Focus = 1 << 2,
		/* Moving to a position, or by a distance */
		PanTiltAbs = 1 << 3,
		PanTiltRel = 1 << 4,
		ZoomAbs = 1 << 5,
		FocusAbs = 1 << 6,
		/* pantilt_home(), and pantilt_set_home() */
		Home = 1 << 7,
		HomeSet = 1 << 8,
		/* set_autofocus(), and the "focus_onetouch" trigger */
		AutoFocus = 1 << 9,
		FocusOneTouch = 1 << 10,
		/* memory_set(), memory_recall() and memory_reset() */
		Presets = 1 << 11,
		/* The "power_on" state key */
		Power = 1 << 12,
		/* The "wb_onepush" trigger */
		WhiteBalanceOnePush = 1 << 13,
		/* The "camera_report" trigger, whose report the
		 * ptz_get_camera_report proc hands back, for working out
		 * what a camera supports */
		Diagnostics = 1 << 14,
		/* The camera has tally lamps, which setTally() lights while the
		 * "tally_auto" setting is on */
		TallyLight = 1 << 15,
	};
	Q_DECLARE_FLAGS(Features, Feature)
	/* Each feature, and its name in the state's "features" */
	static const QList<QPair<Feature, const char *>> &featureNames();

protected:
	std::string type;
	/* Whether to light the camera's tally lamps by itself */
	bool tally_auto = true;
	bool m_tallyProgram = false;
	bool m_tallyPreview = false;
	bool connected = false;
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
	uint m_recallGeneration = 0;
	bool m_frontendCallback = false;
	void onFrontendEvent(enum obs_frontend_event event);
	static void frontendEventCallback(enum obs_frontend_event event, void *data);

protected:
	/* The OBS filter instance that owns this device */
	OBSWeakSource m_filter;
	/* The OBS source this device controls. Weak because the device
	 * doesn't own the source and the user can delete it at any time. */
	mutable OBSWeakSource m_parentSource;
	QString m_parentSourceName;
	mutable QMutex m_parentSourceMutex;
	void watchParentSource(const OBSWeakSource &weak, bool watch) const;
	/* Network drivers use this as the camera's host when their own Host
	 * setting is empty; see parentSourceHost(). Checked when the parent
	 * source is bound and whenever it signals "update". */
	QString m_parentHost;
	/* The parent source's host changed (or was found); "" if it has none.
	 * Not called from the constructor, so a driver reads parentSourceHost()
	 * itself in update(). Must be safe to call with an unchanged host. */
	virtual void onParentHostChanged(const QString &host) { Q_UNUSED(host); }
	/* The camera's presets are the camera's. What the device itself keeps
	 * is a list of Preset, in display order, over both stores */
	struct Preset {
		/* "camera:<the driver's key>" or "local:<key>", see presetId() */
		QString id;
		/* What to show: the user's name for it, or if there is none the camera's */
		QString name;
		/* What the camera calls it, "" if it doesn't */
		QString cameraName;
		/* File name (in ptz_thumbnail_dir()) of its thumbnail, or "" */
		QString thumbnail;
		/* A local preset's values: a recall applies the pan, tilt, zoom and
		 * focus it has */
		OBSData values;
		bool onCamera() const { return id.startsWith(QStringLiteral("camera:")); }
	};
	size_t m_maxPresets = 16;
	QList<Preset> m_presets;
	/* The display order, as ids: apart from m_presets, which has no order of its
	 * own. It can name a preset the camera has yet to say it has. */
	QStringList m_order;
	void reconcileOrder();
	void signalOrderChanged();
	/* What the camera said it has, for a driver that can enumerate, by key and
	 * name. Kept as it is across update()s, which read the saved presets again */
	QList<QPair<QString, QString>> m_cameraPresets;
	bool m_cameraPresetsKnown = false;
	/* Make the list again, from what is saved (a "presets" array) and what the
	 * camera said */
	void loadPresets(obs_data_array_t *saved, obs_data_array_t *order);
	int presetIndex(const QString &id) const;
	/* The current position and focus as a local preset keeps them: only
	 * what the device has reported */
	OBSData captureValues() const;
	void applyValues(const Preset &preset);
	void signalPreset(const char *name, const QString &id);
	void signalPresetChanged(const QString &id, obs_data_t *changed);
	void signalPresetThumbnail(const QString &id);
	void setConnected(bool connected);
	OBSData state;        /* Transient state of the camera. Isn't saved */
	OBSData stateChanged; /* changed state to be sent via the notify signal */
	OBSData statistics;
	QSet<QString> stale_state;
	void incrementStatistic(const char *name, int amount = 1);

	/* Each PTZDevice has a proc handler so methods can be called
	 * from other plugins -- the filter's own */
	proc_handler_t *handler = nullptr;
	/* ...and likewise a signal handler so status changes can sent */
	signal_handler_t *sigs = nullptr;
	/* Registers a proc or signal of the PTZ API on handler or sigs, and logs
	 * it for ptz_registered_api() */
	void addProc(const char *decl, proc_handler_proc_t proc, void *data);
	void addSignal(const char *decl);
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
	/* A driver whose features() change once it is made, as it finds out
	 * what the camera has, calls this to report them */
	void featuresChanged();
	Features reportedFeatures;
	void saveFeatures(obs_data_t *data, Features features) const;

public:
	~PTZDevice();
	PTZDevice(OBSData config, obs_source_t *filter);

	/* Refresh the device's name from its source -- the device's name is
	 * always the name of its source (or the last one it had, while that
	 * source doesn't exist), or the default name if it has never had one.
	 * It is in the state, as "source", and a change is announced there */
	void syncName();
	/* The name of the source the device is on, or the last it was on */
	QString sourceName() const;
	/* Returns a new reference to the device's source (release it with
	 * obs_source_release()), or NULL if it has none. */
	obs_source_t *parentSource() const;
	/* Returns a new reference to the "PTZ Control" filter that owns this
	 * device (release it with obs_source_release()), or NULL if its
	 * filter is gone. */
	obs_source_t *filterSource() const;
	void setParentSource(obs_source_t *source);
	/* The hostname or IP address the parent source reports for the device
	 * it receives from, or "" if it doesn't or there is no parent. Read
	 * from the settings of a DistroAV NDI source ("web_control_url") or
	 * of a browser source showing a page from a server ("url"). */
	QString parentSourceHost() const;
	/* Tells the driver if the parent's host changed since it last looked */
	void checkParentHost();
	/* What the device can do. None, unless a driver says. */
	virtual Features features() const { return {}; }
	/* What the device found out about its camera with its "camera_report"
	 * trigger, for its user to send in, so that cameras like it can be
	 * given what they need: empty until there is a report. Only what
	 * describes the camera, never what identifies it or where it is. */
	virtual QJsonObject cameraReport() const { return {}; }
	/* The tally lamps a camera can have: red while the device's source is in
	 * the program scene, green while it is in the preview scene (Studio
	 * Mode only) and not in the program scene */
	enum class Tally { Program, Preview };
	/* A lamp is to be lit or put out. Nothing here; a driver whose camera
	 * has lamps (the TallyLight feature) does it. Told only when a lamp's
	 * state changes. */
	virtual void setTally(Tally lamp, bool on)
	{
		Q_UNUSED(lamp);
		Q_UNUSED(on);
	}
	/* The frontend's program or preview scene, or Studio Mode, changed, or
	 * the device came to be on a source: work out the lamps again, and
	 * tell setTally() of any that changed. Only for a device with the
	 * TallyLight feature, and while "tally_auto" is on: whoever has it off
	 * has something else drive the lamps, which are then left as they are,
	 * and put right when it is on again. */
	void onSceneChanged();
	/* OBS has finished loading, or is closing and has not yet cleared its
	 * scenes (which destroys the filters that own devices). No-ops here; a
	 * driver that can act on the app itself starting or stopping (turning
	 * the camera's power on or off, say) overrides one or both. A device
	 * hears of both from OBS itself, not through the UI, so it works the
	 * same wherever the device is made. */
	virtual void onOBSStartup() {}
	virtual void onOBSShutdown() {}

	/* Presets, see docs/ptz-device-api.md. An id is "<store>:<key>": the store
	 * is "camera" or "local" (the device's own), and the key is the driver's own
	 * for a camera preset, such as a slot or a token. */
	static QString presetId(const QString &store, const QString &key) { return store + QLatin1Char(':') + key; }
	static QString presetKey(const QString &id) { return id.section(QLatin1Char(':'), 1); }
	using CameraPresets = PTZCameraPresets;
	virtual CameraPresets cameraPresets() const;
	/* The camera store's operations, for a driver whose camera has presets. By
	 * default a camera has numbered slots, which memory_set(), memory_recall()
	 * and memory_reset() use, and its keys are the slot numbers. A driver of a
	 * camera that is not like that overrides these. */
	/* Saves the camera's position in a new preset, and returns its key, or "" if
	 * it can't. May run the event loop for as long as the camera takes to say
	 * what the key is. */
	virtual QString cameraPresetCreate(const QString &name);
	virtual void cameraPresetSave(const QString &key);
	virtual void cameraPresetRecall(const QString &key);
	virtual void cameraPresetDelete(const QString &key);
	/* For a camera that keeps names (CameraPresets::namesOnCamera) */
	virtual void cameraPresetRename(const QString &key, const QString &name)
	{
		Q_UNUSED(key);
		Q_UNUSED(name);
	}
	/* Ask the camera what presets it has, for a driver that can: it answers
	 * with setCameraPresets() */
	virtual void cameraPresetRefresh() {}
	/* What an enumerable camera has, as (key, name) pairs: the presets that the
	 * list has of the camera's are these, and a name the camera changed is
	 * changed. Announces ptz_preset_list_reset if that changed the list. */
	void setCameraPresets(const QList<QPair<QString, QString>> &presets);

	/* The local store: any device that can go to a position has it.
	 * What a preset there holds, by the names of the API's value keys */
	bool localPresets() const;
	QStringList valueKeys() const;
	int presetCount() const { return m_presets.size(); }
	/* Make a preset in `store` from the current position, and return its id, or
	 * "" if it can't be made */
	QString createPreset(const QString &name, const QString &store);
	void savePreset(const QString &id);
	void recallPreset(const QString &id);
	void deletePreset(const QString &id);
	/* Change what `changes` has of "name", "thumbnail" */
	void updatePreset(const QString &id, obs_data_t *changes);
	void movePreset(const QString &id, int index);
	void setPresetThumbnail(const QString &id, const QImage &image);
	void clearPresetThumbnail(const QString &id);
	/* Grabs a frame of the device's source as the preset's thumbnail */
	void capturePresetThumbnail(const QString &id);
	void refreshThumbnailAfterRecall(const QString &id);

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
	 * empty body and leave HomeSet out of features(), so the UI can
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

	/* calldata_t overloads of the query/config/preset-CRUD API below,
	 * registered on the proc_handler so PTZListModel never has to call
	 * these directly -- see PTZDevice::PTZDevice() for registration and
	 * PTZListModel::refreshDeviceState()/refreshPresetList() for callers */
	void get_state(calldata_t *cd) const;
	void get_statistics(calldata_t *cd);
	void get_parent_source(calldata_t *cd) const;
	void request_state(calldata_t *cd);
	void get_camera_report(calldata_t *cd) const;
	void preset_get_list(calldata_t *cd) const;
	void preset_create(calldata_t *cd);
	void preset_delete(calldata_t *cd);
	void preset_update(calldata_t *cd);
	void preset_move(calldata_t *cd);
	void preset_refresh(calldata_t *cd);

public:
	bool isConnected() const { return connected; }
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
	 *     Settings only.
	 * `persistState()`: Write just what the device itself changes, which
	 *     the settings did not give it and so cannot already have, such as
	 *     its presets. `persist()` does it to the settings of the device's
	 *     source, after each change, so that they are the one copy of what
	 *     the device saves; it must not write any setting that can be set
	 *     from outside, which the device may not have been updated with yet.
	 */
	static void defaults(obs_data_t *defaults);
	virtual void update(OBSData ptz_config);
	virtual void save(OBSData ptz_config) const;
	virtual void persistState(obs_data_t *settings) const;
	void persist() const;
	/* `saveDefaults()`: Write defaults that depend on the device, not on its
	 *     type, for the controller to show with its settings: a "<key>:placeholder"
	 *     says what a blank field will use instead. Defaults are never saved,
	 *     and cannot undo a setting. `publish()` writes them to the settings of
	 *     the device's source, as they change. */
	virtual void saveDefaults(obs_data_t *settings) const { Q_UNUSED(settings); }
	void publish() const;

	/* Apply new settings, which the filter's .update does.
	 * `settings` must be complete, as update() requires. */
	void applySettings(OBSData settings);
	/* Say that the device changed its own settings: see PTZDevice::settingsChanged() */
	void settingsChanged();
	/* Remove the keys older versions kept in a filter's settings, "name" and
	 * "id", which the filter does not need: it knows its own source */
	static void stripIdentity(obs_data_t *settings);
	/* Keep a copy of save()'s settings in the rolling backup of devices
	 * that have gone away (see ptz_device_backups_get()). Called when its
	 * source is deleted, its filter is removed, and as it is destroyed. */
	void backup() const;

	/* Properties describe how to display the settings in a GUI dialog */
	virtual obs_properties_t *get_obs_properties();

	/* Transient state: what the camera and the host currently report,
	 * never persisted. The parallel of the settings above:
	 * `saveState()`: fills an OBSData with the device's whole current
	 *     state -- name, connection, live/preview/locked, whatever the
	 *     driver has read back from the camera.
	 * `requestState()`: asks for some state to change. Only the keys
	 *     present are acted on, each by issuing the command that should
	 *     make the camera report it; the value isn't stored here, it
	 *     becomes state when the camera says so (or, where a driver
	 *     already does so, optimistically when the command is sent).
	 *     Commandable keys: focus_af_enabled, and per driver the rest (for
	 *     VISCA, most of what a camera reports: power_on, tally_on,
	 *     tally_preview, wb_mode and its picture and exposure settings,
	 *     see visca_state_commands in ptz-visca.cpp). Anything else is
	 *     ignored, so a caller can hand back state it read, read-only parts
	 *     and all.
	 * Unlike the settings there is no properties tree for it: it isn't
	 * bound to anything OBS persists, and changes many times a second.
	 */
	virtual void saveState(OBSData state) const;
	/* Fills an OBSData with the device's statistics, see ptz_get_statistics.
	 * A driver that has rates works them out here, from what it has
	 * counted since the last time. */
	virtual void saveStatistics(OBSData statistics);
	virtual void requestState(OBSData requested);

	/* One-shot actions on the camera, which aren't state and so have no
	 * place in requestState(), by name: "focus_onetouch" here, and per
	 * driver "wb_onepush" and the diagnostics. Run on the device's own
	 * thread; says whether the device knows the name. */
	virtual bool runTrigger(const QString &name);
};

Q_DECLARE_OPERATORS_FOR_FLAGS(PTZDevice::Features)

/* backend driver hooks that register themselves as an OBS filters */
void *ptz_filter_create(const std::function<PTZDevice *()> &make);
obs_properties_t *ptz_filter_get_properties(void *data);
void ptz_filter_update(void *data, obs_data_t *settings);
void ptz_filter_add(void *data, obs_source_t *parent);
void ptz_filter_remove(void *data, obs_source_t *);
void ptz_filter_destroy(void *data);
void ptz_filter_save(void *data, obs_data_t *settings);

/* What the PTZ API has registered so far, as (scope, declaration) pairs: the
 * procs and signals of docs/ptz-device-api.md. The scope is one of
 * "device-proc" and "device-signal". For tests/ui-harness, to hold that document to it. */
QList<QPair<QString, QString>> ptz_registered_api();
