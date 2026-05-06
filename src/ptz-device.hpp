/* Pan Tilt Zoom device base object
 *
 * Copyright 2020 Grant Likely <grant.likely@secretlab.ca>
 *
 * SPDX-License-Identifier: GPLv2
 */
#pragma once

#include "ptz.h"
#include <qt-wrappers.hpp>
#include <memory>
#include <QObject>
#include <QSet>
#include <QStringListModel>
#include <QtGlobal>
#include <obs.hpp>
#include <obs-frontend-api.h>
#include <util/platform.h>

#define ptz_log(level, format, ...) \
	blog(level, "[%s/%.12s] " format, this->type.c_str(), QT_TO_UTF8(this->objectName()), ##__VA_ARGS__)
#define ptz_info(format, ...) ptz_log(LOG_INFO, format, ##__VA_ARGS__)
#define ptz_debug(format, ...) ptz_log(LOG_DEBUG, format, ##__VA_ARGS__)
#define ptz_debug_trace(format, ...) \
	if (this->protocol_trace)    \
	ptz_log(LOG_DEBUG, format, ##__VA_ARGS__)

class PTZDevice;

class PTZListModel : public QAbstractListModel {
	Q_OBJECT

private:
	static QMap<uint32_t, PTZDevice *> devices;

public:
	enum PTZListModelRole {
		DeviceIdRole = Qt::UserRole,
		DescriptionRole,
		IsLiveRole,
		IsConnectedRole,
		IsLockedRole,
		SupportsSetHomeRole,
	};

	PTZListModel();
	~PTZListModel();
	int rowCount(const QModelIndex &parent = QModelIndex()) const;
	QVariant data(const QModelIndex &index, int role) const;
	bool setData(const QModelIndex &index, const QVariant &value, int role = Qt::EditRole);
	void do_reset();
	void name_changed(PTZDevice *ptz);
	Qt::ItemFlags flags(const QModelIndex &index) const;

	/* Data Model */
	PTZDevice *make_device(OBSData config);
	PTZDevice *getDevice(const QModelIndex &index) const;
	PTZDevice *getDevice(uint32_t device_id) const;
	PTZDevice *getDeviceByName(const QString &name) const;
	QStringList getDeviceNames() const;
	bool callDevice(const QModelIndex &index, const char *method, calldata *cd = nullptr);
	QModelIndex indexFromDeviceId(uint32_t device_id);
	void renameDevice(QString new_name, QString prev_name);
	obs_data_array_t *getConfigs();
	void add(PTZDevice *ptz);
	void remove(PTZDevice *ptz);
	void delete_all();

public slots:
	void move_continuous(uint32_t device_id, uint32_t flags, double pan, double tilt, double zoom, double focus);
};

extern PTZListModel ptzDeviceList;

class PTZPresetListModel : public QAbstractListModel {
	Q_OBJECT

protected:
	/* Collection of all presets, keyed by unique integer id.
	 * On cameras that use preset numbers, the id is mapped 1:1 with the
	 * preset number.  */
	size_t m_maxPresets = 16;
	QMap<size_t, QVariantMap> m_presets;
	QList<size_t> m_displayOrder;
	void sanitize(size_t id);

public:
	/* QAbstractListModel overrides */
	bool insertRows(int row, int count, const QModelIndex &parent = QModelIndex());
	bool removeRows(int row, int count, const QModelIndex &parent = QModelIndex());
	bool moveRows(const QModelIndex &srcParent, int srcRow, int count, const QModelIndex &destParent,
		      int destChild);
	int rowCount(const QModelIndex &parent = QModelIndex()) const;
	QVariant data(const QModelIndex &index, int role) const;
	bool setData(const QModelIndex &index, const QVariant &value, int role = Qt::EditRole);
	Qt::ItemFlags flags(const QModelIndex &index) const;

	/* PTZ Preset API */
	int getPresetId(const QModelIndex &index) const;
	size_t maxPresets() const { return m_maxPresets; };
	void setMaxPresets(size_t max) { m_maxPresets = max; };
	int newPreset();
	QVariant presetProperty(size_t id, QString key);
	bool updatePreset(size_t id, const QVariantMap &map);
	int find(QString key, QVariant value);

	void loadPresets(OBSDataArray preset_array);
	OBSDataArray savePresets() const;
};

class PTZDevice : public QObject {
	Q_OBJECT
	friend class PTZListModel;

protected:
	uint32_t id = 0;
	std::string type;
	bool connected = false;
	bool locked = false;
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

	PTZPresetListModel m_presetsModel;
	void setConnected(bool connected);
	obs_properties_t *props;
	OBSData settings;
	OBSData statistics;
	QSet<QString> stale_settings;
	void incrementStatistic(const char *name);

	// Each PTZ device has a proc handler so methods can be called
	// from other plugins
	proc_handler_t *handler = nullptr;

signals:
	void settingsChanged(OBSData settings);
	void connectionStatusChanged(bool connected);

public:
	~PTZDevice();
	PTZDevice(OBSData config);
	uint32_t getId() { return id; }

	void setObjectName(QString name);
	virtual QString description();
	bool isLive();

	QString presetName(size_t id);
	void setPresetName(size_t id, QString name);

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
protected:
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
	/**
	 * pantilt_set_home(): record the camera's *current* pan/tilt/zoom as
	 * its new home position (so a later pantilt_home() returns here).
	 * Optional — drivers that don't implement it should leave the default
	 * empty body and return false from supportsSetHome(), so the UI can
	 * hide the action entirely instead of presenting a dead control.
	 */
	virtual void pantilt_set_home() {}
	virtual bool supportsSetHome() const { return false; }
	void zoom(double speed);
	virtual void zoom_abs(double pos) { Q_UNUSED(pos); };
	virtual void set_autofocus(bool enabled) { Q_UNUSED(enabled); };
	void focus(double speed);
	virtual void focus_abs(double pos) { Q_UNUSED(pos); }
	virtual void memory_set(int i) { Q_UNUSED(i); }
	virtual void memory_recall(int i) { Q_UNUSED(i); }
	virtual void memory_reset(int i) { Q_UNUSED(i); }

protected slots:
	void stop();
	virtual void pantilt_home() {}
	void move(calldata_t *cd);
	virtual void set(calldata_t *cd);
	virtual void focus_onetouch() {}
	void preset_save(calldata_t *cd);
	void preset_recall(calldata_t *cd);
	void preset_clear(calldata_t *cd);

public:
	virtual QAbstractListModel *presetModel() { return &m_presetsModel; }
	bool isLocked() { return locked; };
	bool isConnected() const { return connected; }
	void setLock(bool state) { locked = state; }
	bool pantiltChanged() const { return pantilt_changed; }
	bool zoomChanged() const { return zoom_changed; }
	bool focusChanged() const { return focus_changed; }

	/* `config` is the device configuration, saved to the config file
	 * `settings` are the dynamic state of the device which includes the
	 * config.  Most of the data in settings is not saved in the config
	 * file. `set_config()` is used to change saved config values, and
	 * `set_settings()` is used to send commands to the camera to change
	 * the state */
	virtual void set_config(OBSData ptz_config);
	virtual OBSData get_config();
	virtual void set_settings(OBSData setting);
	virtual OBSData get_settings();

	/* Properties describe how to display the settings in a GUI dialog */
	virtual obs_properties_t *get_obs_properties();
};
