/* Pan Tilt Zoom device base object
 *
 * Copyright 2020 Grant Likely <grant.likely@secretlab.ca>
 *
 * SPDX-License-Identifier: GPLv2
 */
#pragma once

#include <memory>
#include <QDebug>
#include <QObject>
#include <QStringListModel>
#include <QtGlobal>
#include <obs-frontend-api.h>

class PTZDevice : public QObject {
	Q_OBJECT

private:
	static QStringListModel name_list_model;
	static std::vector<PTZDevice *> devices;

protected:
	std::string type;

public:
	PTZDevice(std::string type) : QObject(), type(type) { devices.push_back(this); };
	~PTZDevice() { };

	static PTZDevice* make_device(obs_data_t *config);
	static PTZDevice* get_device(unsigned int index) { return devices.at(index); }
	static unsigned int device_count() { return devices.size(); }

	virtual void pantilt(double pan, double tilt) { Q_UNUSED(pan); Q_UNUSED(tilt); }
	virtual void pantilt_stop() { }
	virtual void pantilt_home() { }
	virtual void zoom_stop() { }
	virtual void zoom_tele() { }
	virtual void zoom_wide() { }
	static QAbstractListModel * model() { return &name_list_model; }

	virtual void set_config(obs_data_t *ptz_data) {
		const char *name = obs_data_get_string(ptz_data, "name");
		this->setObjectName(name);

		int insert_at = name_list_model.rowCount();
		name_list_model.insertRow(insert_at);
		QModelIndex index = name_list_model.index(insert_at, 0);
		name_list_model.setData(index, name);
	}
	virtual void get_config(obs_data_t *ptz_data) {
		obs_data_set_string(ptz_data, "name", qPrintable(this->objectName()));
		obs_data_set_string(ptz_data, "type", type.c_str());
	}
};

class PTZSimulator : public PTZDevice {
	Q_OBJECT

public:
	PTZSimulator() : PTZDevice("sim") { };
	PTZSimulator(obs_data_t *config) : PTZDevice("sim") { set_config(config); };

	void pantilt(double pan, double tilt) { qDebug() << __func__ << "Pan" << pan << "Tilt" << tilt; }
	void pantilt_stop() { qDebug() << __func__; }
	void pantilt_home() { qDebug() << __func__; }
	void zoom_stop() { qDebug() << __func__; }
	void zoom_tele() { qDebug() << __func__; }
	void zoom_wide() { qDebug() << __func__; }
};
