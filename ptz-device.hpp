/* Pan Tilt Zoom device base object
 *
 * Copyright 2020 Grant Likely <grant.likely@secretlab.ca>
 *
 * SPDX-License-Identifier: GPLv2
 */
#pragma once

#include <QDebug>
#include <QObject>
#include <QStringListModel>
#include <QtGlobal>
#include "../../obs-app.hpp"

class PTZDevice : public QObject {
	Q_OBJECT

private:
	static QStringListModel name_list_model;

protected:
	std::string type;

public:
	PTZDevice(std::string type) : QObject(), type(type) { };
	~PTZDevice() { };

	static PTZDevice* make_device(obs_data_t *config);

	virtual void pantilt(double pan, double tilt) { Q_UNUSED(pan); Q_UNUSED(tilt); }
	virtual void pantilt_stop() { }
	virtual void pantilt_up(uint32_t pan_speed, uint32_t tilt_speed) { Q_UNUSED(pan_speed); Q_UNUSED(tilt_speed); }
	virtual void pantilt_upleft(uint32_t pan_speed, uint32_t tilt_speed) { Q_UNUSED(pan_speed); Q_UNUSED(tilt_speed); }
	virtual void pantilt_upright(uint32_t pan_speed, uint32_t tilt_speed) { Q_UNUSED(pan_speed); Q_UNUSED(tilt_speed); }
	virtual void pantilt_left(uint32_t pan_speed, uint32_t tilt_speed) { Q_UNUSED(pan_speed); Q_UNUSED(tilt_speed); }
	virtual void pantilt_right(uint32_t pan_speed, uint32_t tilt_speed) { Q_UNUSED(pan_speed); Q_UNUSED(tilt_speed); }
	virtual void pantilt_down(uint32_t pan_speed, uint32_t tilt_speed) { Q_UNUSED(pan_speed); Q_UNUSED(tilt_speed); }
	virtual void pantilt_downleft(uint32_t pan_speed, uint32_t tilt_speed) { Q_UNUSED(pan_speed); Q_UNUSED(tilt_speed); }
	virtual void pantilt_downright(uint32_t pan_speed, uint32_t tilt_speed) { Q_UNUSED(pan_speed); Q_UNUSED(tilt_speed); }
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
	void pantilt_up(uint32_t pan_speed, uint32_t tilt_speed) { qDebug() << __func__ << "Pan" << pan_speed << "Tilt" << tilt_speed; }
	void pantilt_upleft(uint32_t pan_speed, uint32_t tilt_speed) { qDebug() << __func__ << "Pan" << pan_speed << "Tilt" << tilt_speed; }
	void pantilt_upright(uint32_t pan_speed, uint32_t tilt_speed) { qDebug() << __func__ << "Pan" << pan_speed << "Tilt" << tilt_speed; }
	void pantilt_left(uint32_t pan_speed, uint32_t tilt_speed) { qDebug() << __func__ << "Pan" << pan_speed << "Tilt" << tilt_speed; }
	void pantilt_right(uint32_t pan_speed, uint32_t tilt_speed) { qDebug() << __func__ << "Pan" << pan_speed << "Tilt" << tilt_speed; }
	void pantilt_down(uint32_t pan_speed, uint32_t tilt_speed) { qDebug() << __func__ << "Pan" << pan_speed << "Tilt" << tilt_speed; }
	void pantilt_downleft(uint32_t pan_speed, uint32_t tilt_speed) { qDebug() << __func__ << "Pan" << pan_speed << "Tilt" << tilt_speed; }
	void pantilt_downright(uint32_t pan_speed, uint32_t tilt_speed) { qDebug() << __func__ << "Pan" << pan_speed << "Tilt" << tilt_speed; }
	void pantilt_home() { qDebug() << __func__; }
	void zoom_stop() { qDebug() << __func__; }
	void zoom_tele() { qDebug() << __func__; }
	void zoom_wide() { qDebug() << __func__; }
};
