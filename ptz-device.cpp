/* Pan Tilt Zoom device factory
 *
 * Copyright 2020 Grant Likely <grant.likely@secretlab.ca>
 *
 * SPDX-License-Identifier: GPLv2
 */

#include "ptz-device.hpp"
#include "ptz-visca.hpp"

PTZListModel PTZDevice::ptz_list_model;
QVector<PTZDevice *> PTZDevice::devices;

int PTZListModel::rowCount(const QModelIndex& parent) const
{
	return PTZDevice::device_count();
}

Qt::ItemFlags PTZListModel::flags(const QModelIndex &index) const
{
	if (!index.isValid())
		return Qt::ItemIsEnabled;
	return QAbstractListModel::flags(index) | Qt::ItemIsEditable;
}

QVariant PTZListModel::data(const QModelIndex &index, int role) const
{
	if (!index.isValid())
		return QVariant();

	if (role == Qt::DisplayRole || role == Qt::EditRole) {
		return PTZDevice::get_device(index.row())->objectName();
	}
#if 0
	if (role == Qt::UserRole) {
		return PTZDevice::get_device(index.row());
	}
#endif
	return QVariant();
}

bool PTZListModel::setData(const QModelIndex &index, const QVariant &value, int role)
{
	if (index.isValid() && role == Qt::EditRole) {
		PTZDevice *ptz = PTZDevice::get_device(index.row());
		if (ptz)
			ptz->setObjectName(value.toString());
		emit dataChanged(index, index);
	}
	return false;
}

PTZDevice *PTZDevice::get_device_by_name(QString &name)
{
	for (auto ptz : devices)
		if (name == ptz->objectName())
			return ptz;
	return NULL;
}

PTZDevice *PTZDevice::make_device(obs_data_t *config)
{
	PTZDevice *ptz = nullptr;
	std::string type = obs_data_get_string(config, "type");

	if (type == "sim")
		ptz = new PTZSimulator(config);
	if (type == "visca")
		ptz = new PTZVisca(config);
	return ptz;
}
