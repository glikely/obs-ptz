/* Pan Tilt Zoom Controls - PTZDevice base object
 *
 * Copyright 2020-2026 Grant Likely <grant.likely@secretlab.ca>
 *
 * SPDX-License-Identifier: GPLv2
 */
#pragma once

#include <QObject>
#include <QList>
#include <QMap>
#include <QVariantMap>
#include <obs.hpp>
#include <obs-frontend-api.h>
#include <qt-wrappers.hpp>
#include <util/platform.h>
#include "ptz.h"

#define ptz_log(level, format, ...) \
	blog(level, "[%s/%.12s] " format, this->type.c_str(), QT_TO_UTF8(this->objectName()), ##__VA_ARGS__)
#define ptz_info(format, ...) ptz_log(LOG_INFO, format, ##__VA_ARGS__)
#define ptz_debug(format, ...) ptz_log(LOG_DEBUG, format, ##__VA_ARGS__)
#define ptz_debug_trace(format, ...) \
	if (this->protocol_trace)    \
	ptz_log(LOG_DEBUG, format, ##__VA_ARGS__)

struct ptz_filter;

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

protected:
	/* Collection of all presets, keyed by unique integer id.
	 * On cameras that use preset numbers, the id is mapped 1:1 with the
	 * preset number.  */
	size_t m_maxPresets = 16;
	QMap<size_t, QVariantMap> m_presets;
	QList<size_t> m_presetsDisplayOrder;
	void sanitizePreset(size_t id);
	void setConnected(bool connected);
	obs_properties_t *props;
	OBSData settings;
	OBSData statistics;
	QSet<QString> stale_settings;
	void incrementStatistic(const char *name);

	/* The owning "PTZ Control" filter's own source -- never the parent.
	 * Set at construction and never reassigned. handler/sigs below are
	 * borrowed from this source's own proc_handler/signal_handler (every
	 * obs_source_t has one), not separately allocated. */
	obs_source_t *filter_source = nullptr;
	// Each PTZ device is controlled via its filter source's proc handler,
	// so methods can be called from other plugins
	proc_handler_t *handler = nullptr;
	// ...and observed via its filter source's signal handler, so status
	// changes don't need a direct C++ reference to this class (see
	// ptz-list-model.cpp)
	signal_handler_t *sigs = nullptr;
	void notifySettingsChanged();

public:
	~PTZDevice();
	PTZDevice(OBSData config, obs_source_t *filter_source);
	uint32_t getId() const { return id; }
	std::string getType() const { return type; }
	/* Fires the create signal PTZListModel discovers new devices through.
	 * Called by ptz_device_create()'s caller once the returned object is
	 * both fully constructed and assigned to ptzf->ptz -- see the
	 * comment on the definition. */
	void announceCreated();
	/* Registers every ptz_* proc_handler entry and signal declaration on
	 * ptzf->source's own proc_handler/signal_handler, once for the
	 * filter's whole lifetime (called from ptz_filter_create(), not from
	 * PTZDevice's own constructor) -- see the comment on the definition
	 * for why a per-PTZDevice-instance registration would be unsafe. A
	 * static member (not a free function) purely so its lambdas keep
	 * access to the protected calldata_t methods below, the same way
	 * they would from inside the constructor. */
	static void registerFilterHandlers(struct ptz_filter *ptzf);

	void setObjectName(QString name);
	virtual QString description();
	bool isLive() const { return live; }
	bool isPreview() const { return preview; }
	virtual bool supportsSetHome() const { return false; }
	void onSceneChanged();
	/* The source this device controls, found via the owning filter's real
	 * association (obs_filter_get_parent()) rather than by matching names.
	 * Addref'd like obs_get_source_by_name() -- caller must release.
	 * Returns nullptr if the filter hasn't been attached to a parent yet
	 * (e.g. mid ptz_filter_create(), before obs_source_filter_add() runs). */
	obs_source_t *getSource() const;
	/* Called once, right after the owning filter is attached to a parent
	 * source (see ptz_filter_add() in ptz-device.cpp) -- gives a freshly
	 * created device a real name instead of sitting at the placeholder
	 * default forever, since there's no more "associated source" combo to
	 * do this job. */
	void onFilterAddedToSource(obs_source_t *parent);

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
	virtual void get(calldata_t *cd) const;
	virtual void set(calldata_t *cd);
	void preset_save(calldata_t *cd);
	void preset_recall(calldata_t *cd);
	void preset_clear(calldata_t *cd);
	/* Invoked (via a queued connection, since the type change that
	 * triggers this happens off PTZDevice's own construction, inside
	 * ptz_filter_update()) to ask the owning filter to refresh its
	 * properties dialog after a type change swaps in a new PTZDevice --
	 * see ptz_filter_update() in ptz-device.cpp. */
	void notify_properties_changed();

	/* calldata_t overloads of the query/config/preset-CRUD API below,
	 * registered on the proc_handler so PTZListModel never has to call
	 * these directly -- see PTZDevice::PTZDevice() for registration and
	 * PTZListModel::refreshDeviceState()/refreshPresetList() for callers */
	void get_state(calldata_t *cd);
	void setObjectName(calldata_t *cd);
	void setLock(calldata_t *cd);
	void get_config(calldata_t *cd) const;
	void set_config(calldata_t *cd);
	void get_obs_properties(calldata_t *cd);
	void preset_get_list(calldata_t *cd) const;
	void newPreset(calldata_t *cd);
	void removePresetAtDisplayRow(calldata_t *cd);
	void movePreset(calldata_t *cd);
	void setPresetName(calldata_t *cd);
	void onSceneChanged(calldata_t *cd) { Q_UNUSED(cd); onSceneChanged(); }

public:
	bool isLocked() const { return locked; };
	bool isConnected() const { return connected; }
	void setLock(bool state);
	bool pantiltChanged() const { return pantilt_changed; }
	bool zoomChanged() const { return zoom_changed; }
	bool focusChanged() const { return focus_changed; }

	/* Device configuration methods
	 * These match the pattern used by sources in OBS studio with the following methods:
	 * `getDefaults()`: loads OBSData with default values for the device
	 * `update()`: which informs the device of changes to the configuration
	 * `save()`: Make sure device configuration is written to an OBSData
	 */
	virtual void getDefaults(OBSData defaults) const;
	virtual void update(OBSData ptz_config);
	virtual void save(OBSData ptz_config) const;

	/* Properties describe how to display the settings in a GUI dialog */
	virtual obs_properties_t *get_obs_properties();
};
