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

class PTZListModel : public QAbstractListModel {
	Q_OBJECT

public:
	int rowCount(const QModelIndex& parent = QModelIndex()) const;
	QVariant data(const QModelIndex &index, int role) const;
	void do_reset() { beginResetModel(); endResetModel(); }
	Qt::ItemFlags flags(const QModelIndex &index) const;
	bool setData(const QModelIndex &index, const QVariant &value, int role);
};

class PTZDevice : public QObject {
	Q_OBJECT

private:
	static PTZListModel ptz_list_model;
	static QVector<PTZDevice *> devices;

protected:
	std::string type;

public:
	PTZDevice(std::string type) : QObject(), type(type)
	{
		devices.push_back(this);
		ptz_list_model.do_reset();
	};
	~PTZDevice()
	{
		int row = devices.indexOf(this);
		if (row < 0)
			return;
		devices.remove(row);
		ptz_list_model.do_reset();
	};

	static PTZDevice* make_device(obs_data_t *config);
	static PTZDevice* get_device(unsigned int index) { return devices.at(index); }
	static PTZDevice* get_device_by_name(QString &name);
	static unsigned int device_count() { return devices.size(); }

	virtual void pantilt(double pan, double tilt) { Q_UNUSED(pan); Q_UNUSED(tilt); }
	virtual void pantilt_stop() { }
	virtual void pantilt_home() { }
	virtual void zoom_stop() { }
	virtual void zoom_tele() { }
	virtual void zoom_wide() { }
	static QAbstractListModel * model() { return &ptz_list_model; }

	virtual void setObjectName(QString name) {
		name = name.simplified();
		if (name == objectName())
			return;
		if (name == "")
			name = "Unnamed Device";
		QString new_name = name;
		for (int i = 1;; i++) {
			PTZDevice *ptz = get_device_by_name(new_name);
			if (!ptz)
				break;
			new_name = name + " " + QString::number(i);
			qDebug() << "new values" << new_name << name << i;
		}
		QObject::setObjectName(new_name);
	}

	virtual void set_config(obs_data_t *ptz_data) {
		const char *name = obs_data_get_string(ptz_data, "name");
		setObjectName(name);
		ptz_list_model.do_reset();
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
