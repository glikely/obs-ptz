/* Pan Tilt Zoom device factory
 *
 * Copyright 2020 Grant Likely <grant.likely@secretlab.ca>
 *
 * SPDX-License-Identifier: GPLv2
 */

#include "ptz-device.hpp"
#include "ptz-visca.hpp"

PTZListModel PTZDevice::ptz_list_model;
std::vector<PTZDevice *> PTZDevice::devices;

int PTZListModel::rowCount(const QModelIndex& parent) const
{
	return PTZDevice::device_count();
}

QVariant PTZListModel::data(const QModelIndex &index, int role) const
{
	if (!index.isValid())
		return QVariant();

	if (role == Qt::DisplayRole) {
		return PTZDevice::get_device(index.row())->objectName();
	}
#if 0
	if (role == Qt::UserRole) {
		return PTZDevice::get_device(index.row());
	}
#endif
	return QVariant();
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
