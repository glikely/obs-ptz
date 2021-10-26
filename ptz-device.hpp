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

extern int ptz_debug_level;
#define ptz_debug(format, ...) \
	blog(ptz_debug_level, "%s():%i: " format, __FUNCTION__, __LINE__, \
	##__VA_ARGS__)

class PTZDevice;

class PTZListModel : public QAbstractListModel {
	Q_OBJECT

private:
	static QMap<uint32_t, PTZDevice *> devices;

public:
	int rowCount(const QModelIndex& parent = QModelIndex()) const;
	QVariant data(const QModelIndex &index, int role) const;
	void do_reset() { beginResetModel(); endResetModel(); }
	Qt::ItemFlags flags(const QModelIndex &index) const;
	bool setData(const QModelIndex &index, const QVariant &value, int role);

	/* Data Model */
	PTZDevice* make_device(OBSData config);
	PTZDevice* getDevice(const QModelIndex &index);
	uint32_t getDeviceId(const QModelIndex &index);
	PTZDevice* getDevice(uint32_t device_id);
	PTZDevice* getDeviceByName(QString &name);
	QModelIndex indexFromDeviceId(uint32_t device_id);
	obs_data_array_t* getConfigs();
	void add(PTZDevice *ptz);
	void remove(PTZDevice *ptz);
	unsigned int device_count() { return devices.size(); }
	void delete_all();

public slots:
	void preset_recall(uint32_t device_id, int preset_id);
	void pantilt(uint32_t device_id, double pan, double tilt);
};

extern PTZListModel ptzDeviceList;

const QStringList default_preset_names({"Preset 1", "Preset 2", "Preset 3", "Preset 4",
	"Preset 5",  "Preset 6",  "Preset 7",  "Preset 8",  "Preset 9",  "Preset 10",
	"Preset 11", "Preset 12", "Preset 13", "Preset 14", "Preset 15", "Preset 16"});

class PTZDevice : public QObject {
	Q_OBJECT
	friend class PTZListModel;

protected:
	uint32_t id = 0;
	std::string type;
	QStringList auto_settings_filter = {"name", "type"};

	QStringListModel preset_names_model;
	obs_properties_t *props;
	OBSData settings;

signals:
	void settingsChanged(OBSData settings);

public:
	~PTZDevice();
	PTZDevice(OBSData config);
	uint32_t getId() { return id; }

	void setObjectName(QString name);

	virtual void pantilt(double pan, double tilt) { Q_UNUSED(pan); Q_UNUSED(tilt); }
	virtual void pantilt_rel(int pan, int tilt) { Q_UNUSED(pan); Q_UNUSED(tilt); }
	virtual void pantilt_abs(int pan, int tilt) { Q_UNUSED(pan); Q_UNUSED(tilt); }
	virtual void pantilt_stop() { }
	virtual void pantilt_home() { }
	virtual void zoom_stop() { }
	virtual void zoom_tele(double speed) { Q_UNUSED(speed); }
	virtual void zoom_wide(double speed) { Q_UNUSED(speed); }
	virtual void zoom_abs(int pos) { Q_UNUSED(pos); };
	virtual void set_autofocus(bool enabled) { Q_UNUSED(enabled); };
	virtual void focus_stop() { }
	virtual void focus_near(double speed) { Q_UNUSED(speed); }
	virtual void focus_far(double speed) { Q_UNUSED(speed); }
	virtual void focus_onetouch() { }
	virtual void memory_set(int i) { Q_UNUSED(i); }
	virtual void memory_recall(int i) { Q_UNUSED(i); }
	virtual void memory_reset(int i) { Q_UNUSED(i); }
	virtual QAbstractListModel * presetModel() { return &preset_names_model; }

	/* `config` is the device configuration, saved to the config file
	 * `settings` are the dynamic state of the device which includes the
	 * config.  Most of the data in settings is not saved in the config
	 * file. `set_config()` is used to change saved config values, and
	 * `set_settings()` is used to send commands to the camera to change
	 * the state */
	virtual void set_config(OBSData ptz_config);
	virtual OBSData get_config();
	virtual void set_settings(OBSData setting) {}
	virtual OBSData get_settings();

	/* Properties describe how to display the settings in a GUI dialog */
	virtual obs_properties_t *get_obs_properties();
};
