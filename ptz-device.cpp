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

void PTZDevice::set_config(obs_data_t *ptz_config)
{
	config = ptz_config;
	obs_data_addref(config);
	setObjectName(obs_data_get_string(config, "name"));
	ptz_list_model.do_reset();

	/* Update the list of preset names */
	obs_data_array_t *preset_array = obs_data_get_array(config, "presets");
	if (preset_array) {
		QStringList preset_names = default_preset_names;
		for (int i = 0; i < obs_data_array_count(preset_array); i++) {
			obs_data_t *preset = obs_data_array_item(preset_array, i);
			if (!preset)
				continue;
			int preset_id = obs_data_get_int(preset, "id");
			const char *preset_name = obs_data_get_string(preset, "name");
			if ((preset_id >= 0) && (preset_id < preset_names.size()) && preset_name)
					preset_names[preset_id] = preset_name;
		}
		preset_names_model.setStringList(preset_names);
	}
}

obs_data_t *PTZDevice::get_config()
{
	QStringList list = preset_names_model.stringList();
	obs_data_addref(config);
	obs_data_array_t *preset_array = obs_data_array_create();
	for (int i = 0; i < list.size(); i++) {
		obs_data_t *preset = obs_data_create();
		obs_data_set_int(preset, "id", i);
		obs_data_set_string(preset, "name", qPrintable(list[i]));
		obs_data_array_push_back(preset_array, preset);
	}
	obs_data_set_array(config, "presets", preset_array);
	return config;
}
