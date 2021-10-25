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
	static QVector<PTZDevice *> devices;

public:
	int rowCount(const QModelIndex& parent = QModelIndex()) const;
	QVariant data(const QModelIndex &index, int role) const;
	void do_reset() { beginResetModel(); endResetModel(); }
	Qt::ItemFlags flags(const QModelIndex &index) const;
	bool setData(const QModelIndex &index, const QVariant &value, int role);

	/* Data Model */
	PTZDevice* make_device(OBSData config);
	PTZDevice* get_device(unsigned int index) { return devices.value(index, nullptr); }
	PTZDevice* get_device_by_name(QString &name);
	void add(PTZDevice *ptz) { devices.push_back(ptz); do_reset(); }
	void remove(PTZDevice *ptz) {
		int row = devices.indexOf(ptz);
		if (row < 0)
			return;
		devices.remove(row);
		do_reset();
	}
	unsigned int device_count() { return devices.size(); }
	void delete_all();

public slots:
	void preset_recall(int id, int preset_id);
	void pantilt(int id, double pan, double tilt);
};

extern PTZListModel ptzDeviceList;

const QStringList default_preset_names({"Preset 1", "Preset 2", "Preset 3", "Preset 4",
	"Preset 5",  "Preset 6",  "Preset 7",  "Preset 8",  "Preset 9",  "Preset 10",
	"Preset 11", "Preset 12", "Preset 13", "Preset 14", "Preset 15", "Preset 16"});

class PTZDevice : public QObject {
	Q_OBJECT

protected:
	std::string type;
	QStringList auto_settings_filter = {"name", "type"};

	QStringListModel preset_names_model;
	obs_properties_t *props;
	OBSData settings;

signals:
	void settingsChanged(OBSData settings);

public:
	PTZDevice(OBSData config) : QObject()
	{
		type = obs_data_get_string(config, "type");
		settings = obs_data_create();
		obs_data_release(settings);
		ptzDeviceList.add(this);
		preset_names_model.setStringList(default_preset_names);
	};
	~PTZDevice()
	{
		ptzDeviceList.remove(this);
	};

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

	virtual void setObjectName(QString name) {
		name = name.simplified();
		if (name == objectName())
			return;
		if (name == "")
			name = "Unnamed Device";
		QString new_name = name;
		for (int i = 1;; i++) {
			PTZDevice *ptz = ptzDeviceList.get_device_by_name(new_name);
			if (!ptz)
				break;
			new_name = name + " " + QString::number(i);
			ptz_debug("new name %s", qPrintable(new_name));
		}
		QObject::setObjectName(new_name);
	}
};
