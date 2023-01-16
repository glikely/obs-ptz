/* Pan Tilt Zoom device base object
 *
 * Copyright 2020 Grant Likely <grant.likely@secretlab.ca>
 *
 * SPDX-License-Identifier: GPLv2
 */
#pragma once

#include "ptz.h"
#include <memory>
#include <QObject>
#include <QStringListModel>
#include <QtGlobal>
#include <obs.hpp>
#include <obs-frontend-api.h>
#include <util/platform.h>

extern int ptz_debug_level;
#define ptz_info(format, ...) blog(LOG_INFO, format, ##__VA_ARGS__)
#define ptz_debug(format, ...)                                            \
	blog(ptz_debug_level, "%s():%i: " format, __FUNCTION__, __LINE__, \
	     ##__VA_ARGS__)

class PTZDevice;

class PTZListModel : public QAbstractListModel {
	Q_OBJECT

private:
	static QMap<uint32_t, PTZDevice *> devices;

public:
	PTZListModel();
	~PTZListModel();
	int rowCount(const QModelIndex &parent = QModelIndex()) const;
	QVariant data(const QModelIndex &index, int role) const;
	void do_reset();
	void name_changed(PTZDevice *ptz);
	Qt::ItemFlags flags(const QModelIndex &index) const;

	/* Data Model */
	PTZDevice *make_device(OBSData config);
	PTZDevice *getDevice(const QModelIndex &index);
	uint32_t getDeviceId(const QModelIndex &index);
	PTZDevice *getDevice(uint32_t device_id);
	PTZDevice *getDeviceByName(const QString &name);
	QStringList getDeviceNames();
	QModelIndex indexFromDeviceId(uint32_t device_id);
	void renameDevice(QString new_name, QString prev_name);
	obs_data_array_t *getConfigs();
	void add(PTZDevice *ptz);
	void remove(PTZDevice *ptz);
	void delete_all();

public slots:
	void preset_recall(uint32_t device_id, int preset_id);
	void move_continuous(uint32_t device_id, uint32_t flags, double pan,
			     double tilt, double zoom, double focus);
};

extern PTZListModel ptzDeviceList;

const QStringList default_preset_names(
	{"Preset 1", "Preset 2", "Preset 3", "Preset 4", "Preset 5", "Preset 6",
	 "Preset 7", "Preset 8", "Preset 9", "Preset 10", "Preset 11",
	 "Preset 12", "Preset 13", "Preset 14", "Preset 15", "Preset 16"});

class PTZDevice : public QObject {
	Q_OBJECT
	friend class PTZListModel;

public:
	enum StatusFlags {
		STATUS_CONNECTED = 0x1,
		STATUS_PANTILT_SPEED_CHANGED = 0x2,
		STATUS_ZOOM_SPEED_CHANGED = 0x4,
		STATUS_FOCUS_SPEED_CHANGED = 0x8,
	};

protected:
	uint32_t id = 0;
	std::string type;
	uint32_t status = 0;
	double pan_speed = 0;
	double tilt_speed = 0;
	double zoom_speed = 0;
	double focus_speed = 0;

	QStringListModel preset_names_model;
	obs_properties_t *props;
	OBSData settings;
	QSet<QString> stale_settings;

signals:
	void settingsChanged(OBSData settings);
	void propertiesUpdated(OBSData settings);

public:
	~PTZDevice();
	PTZDevice(OBSData config);
	uint32_t getId() { return id; }

	void setObjectName(QString name);
	virtual QString description();

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
	void pantilt(double pan, double tilt)
	{
		if ((pan_speed == pan) && (tilt_speed == tilt))
			return;
		pan_speed = pan;
		tilt_speed = tilt;
		status |= STATUS_PANTILT_SPEED_CHANGED;
		do_update();
	}
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
	void zoom(double speed)
	{
		if (zoom_speed == speed)
			return;
		zoom_speed = speed;
		status |= STATUS_ZOOM_SPEED_CHANGED;
		do_update();
	}
	virtual void zoom_abs(double pos) { Q_UNUSED(pos); };
	virtual void set_autofocus(bool enabled) { Q_UNUSED(enabled); };
	void focus(double speed)
	{
		if (focus_speed == speed)
			return;
		focus_speed = speed;
		status |= STATUS_FOCUS_SPEED_CHANGED;
		do_update();
	}
	virtual void focus_abs(double pos) { Q_UNUSED(pos); }
	virtual void focus_onetouch() {}
	virtual void memory_set(int i) { Q_UNUSED(i); }
	virtual void memory_recall(int i) { Q_UNUSED(i); }
	virtual void memory_reset(int i) { Q_UNUSED(i); }
	virtual QAbstractListModel *presetModel()
	{
		return &preset_names_model;
	}

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
