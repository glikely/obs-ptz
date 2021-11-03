/* Pan Tilt Zoom device factory
 *
 * Copyright 2020 Grant Likely <grant.likely@secretlab.ca>
 *
 * SPDX-License-Identifier: GPLv2
 */

#include <obs.hpp>
#include "imported/qt-wrappers.hpp"
#include "ptz-device.hpp"
#include "ptz-visca.hpp"
#include "ptz-pelco.hpp"
#include "ptz.h"

int ptz_debug_level = LOG_INFO;

PTZListModel ptzDeviceList;
QMap<uint32_t, PTZDevice *> PTZListModel::devices;

static void source_rename_cb(void *data, calldata_t *cd)
{
	auto ptzlm = static_cast<PTZListModel*>(data);
	ptzlm->renameDevice(calldata_string(cd, "new_name"),
			    calldata_string(cd, "prev_name"));
}

void PTZListModel::renameDevice(QString new_name, QString prev_name)
{
	auto ptz = getDeviceByName(prev_name);
	if (ptz)
		ptz->setObjectName(new_name);
}

PTZListModel::PTZListModel()
{
	signal_handler_t *sh = obs_get_signal_handler();
	signal_handler_connect(sh, "source_rename", source_rename_cb, this);
}

PTZListModel::~PTZListModel()
{
	signal_handler_t *sh = obs_get_signal_handler();
	signal_handler_disconnect(sh, "source_rename", source_rename_cb, this);
}

int PTZListModel::rowCount(const QModelIndex& parent) const
{
	Q_UNUSED(parent);
	return devices.size();
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
		return devices.value(devices.keys().at(index.row()))->objectName();
	}
#if 0
	if (role == Qt::UserRole) {
		return get_device(index.row());
	}
#endif
	return QVariant();
}

bool PTZListModel::setData(const QModelIndex &index, const QVariant &value, int role)
{
	if (index.isValid() && role == Qt::EditRole) {
		PTZDevice *ptz = ptzDeviceList.getDevice(index);
		if (ptz)
			ptz->setObjectName(value.toString());
		emit dataChanged(index, index);
	}
	return false;
}

PTZDevice *PTZListModel::getDevice(const QModelIndex &index)
{
	if (index.row() < 0)
		return nullptr;
	return (devices.constBegin() + index.row()).value();
}

uint32_t PTZListModel::getDeviceId(const QModelIndex &index)
{
	if (index.row() < 0)
		return 0;
	return (devices.constBegin() + index.row()).key();
}

PTZDevice *PTZListModel::getDevice(uint32_t device_id)
{
	return devices.value(device_id, nullptr);
}

PTZDevice *PTZListModel::getDeviceByName(const QString &name)
{
	for (auto key : devices.keys()) {
		auto ptz = devices.value(key);
		if (name == ptz->objectName())
			return ptz;
	}
	return NULL;
}

QStringList PTZListModel::getDeviceNames()
{
	QStringList names;
	for (auto key : devices.keys())
		names.append(devices.value(key)->objectName());
	return names;
}

QModelIndex PTZListModel::indexFromDeviceId(uint32_t device_id)
{
	int row = devices.keys().indexOf(device_id);
	if (row < 0)
		return QModelIndex();
	return index(row, 0);
}

obs_data_array_t *PTZListModel::getConfigs()
{
	obs_data_array_t *configs = obs_data_array_create();
	for (auto key : devices.keys())
		obs_data_array_push_back(configs, ptzDeviceList.getDevice(key)->get_config());
	return configs;
}

void PTZListModel::add(PTZDevice *ptz)
{
	/* Assign a unique ID */
	if (ptz->id == 0 || devices.contains(ptz->id))
		ptz->id = devices.isEmpty() ? 1 : devices.lastKey() + 1;
	while (devices.contains(ptz->id)) {
		ptz->id++;
		if (ptz->id == 0)
			ptz->id++;
	}
	devices.insert(ptz->id, ptz);
	do_reset();
}

void PTZListModel::remove(PTZDevice *ptz)
{
	if (ptz == devices.value(ptz->id)) {
		devices.remove(ptz->id);
		do_reset();
	}
}

PTZDevice *PTZListModel::make_device(OBSData config)
{
	PTZDevice *ptz = nullptr;
	std::string type = obs_data_get_string(config, "type");

	if (type == "pelco" || type == "pelco-p")
		ptz = new PTZPelco(config);
	if (type == "visca")
		ptz = new PTZViscaSerial(config);
	if (type == "visca-over-ip")
		ptz = new PTZViscaOverIP(config);
	if (type == "visca-over-tcp")
		ptz = new PTZViscaOverTCP(config);
	return ptz;
}

void PTZListModel::delete_all()
{
	// Devices remove themselves when deleted, so just loop until empty
	while (!devices.isEmpty())
		delete devices.first();
}

void PTZListModel::preset_recall(uint32_t device_id, int preset_id)
{
	PTZDevice* ptz = ptzDeviceList.getDevice(device_id);
	if (ptz)
		ptz->memory_recall(preset_id);
}

void PTZListModel::pantilt(uint32_t device_id, double pan, double tilt)
{
	PTZDevice* ptz = ptzDeviceList.getDevice(device_id);
	if (ptz)
		ptz->pantilt(pan, tilt);
}


PTZDevice::PTZDevice(OBSData config) : QObject()
{
	setObjectName(obs_data_get_string(config, "name"));
	id = obs_data_get_int(config, "id");
	type = obs_data_get_string(config, "type");
	settings = obs_data_create();
	obs_data_release(settings);
	ptzDeviceList.add(this);
	preset_names_model.setStringList(default_preset_names);
};

PTZDevice::~PTZDevice()
{
	ptzDeviceList.remove(this);
};

void PTZDevice::setObjectName(QString name)
{
	name = name.simplified();
	if (name == "")
		name = "PTZ Device";
	if (name == objectName())
		return;
	QString new_name = name;
	for (int i = 1;; i++) {
		PTZDevice *ptz = ptzDeviceList.getDeviceByName(new_name);
		if (!ptz)
			break;
		new_name = name + " " + QString::number(i);
		ptz_debug("new name %s", qPrintable(new_name));
	}
	QObject::setObjectName(new_name);
	ptzDeviceList.do_reset();
}

void PTZDevice::set_config(OBSData config)
{
	/* Update the list of preset names */
	OBSDataArray preset_array = obs_data_get_array(config, "presets");
	obs_data_array_release(preset_array);
	if (preset_array) {
		QStringList preset_names = default_preset_names;
		for (size_t i = 0; i < obs_data_array_count(preset_array); i++) {
			OBSData preset = obs_data_array_item(preset_array, i);
			obs_data_release(preset);
			int preset_id = obs_data_get_int(preset, "id");
			const char *preset_name = obs_data_get_string(preset, "name");
			if ((preset_id >= 0) && (preset_id < preset_names.size()) && preset_name)
					preset_names[preset_id] = preset_name;
		}
		preset_names_model.setStringList(preset_names);
	}
}

OBSData PTZDevice::get_config()
{
	OBSData config = obs_data_create();
	obs_data_release(config);

	obs_data_set_string(config, "name", QT_TO_UTF8(objectName()));
	obs_data_set_int(config, "id", id);
	obs_data_set_string(config, "type", type.c_str());
	QStringList list = preset_names_model.stringList();
	OBSDataArray preset_array = obs_data_array_create();
	obs_data_array_release(preset_array);
	for (int i = 0; i < list.size(); i++) {
		OBSData preset = obs_data_create();
		obs_data_release(preset);
		obs_data_set_int(preset, "id", i);
		obs_data_set_string(preset, "name", QT_TO_UTF8(list[i]));
		obs_data_array_push_back(preset_array, preset);
	}
	obs_data_set_array(config, "presets", preset_array);
	return config;
}

OBSData PTZDevice::get_settings()
{
	obs_data_apply(settings, get_config());
	return settings;
}

obs_properties_t *PTZDevice::get_obs_properties()
{
	obs_properties_t *props = obs_properties_create();
	obs_properties_t *config = obs_properties_create();
	obs_properties_add_group(props, "interface", "Connection", OBS_GROUP_NORMAL, config);

	for (obs_data_item_t *item = obs_data_first(settings); item; obs_data_item_next(&item)) {
		enum obs_data_type type = obs_data_item_gettype(item);
		const char *name = obs_data_item_get_name(item);
		if (auto_settings_filter.contains(name))
			continue;
		obs_property_t *p = nullptr;

		switch (type) {
		case OBS_DATA_BOOLEAN:
			p = obs_properties_add_bool(props, name, name);
			break;
		case OBS_DATA_NUMBER:
			p = obs_properties_add_int(props, name, name, INT_MIN, INT_MAX, 1);
			break;
		case OBS_DATA_STRING:
			p = obs_properties_add_text(props, name, name, OBS_TEXT_DEFAULT);
			break;
		default:
			break;
		}
		if (p)
			obs_property_set_enabled(p, false);
	}

	return props;
}

/* C interface for non-QT parts of the plugin */
obs_data_array_t *ptz_devices_get_config()
{
	return ptzDeviceList.getConfigs();
}

void ptz_devices_set_config(obs_data_array_t *devices)
{
	if (!devices) {
		blog(LOG_INFO, "No PTZ device configuration found");
		return;
	}
	for (size_t i = 0; i < obs_data_array_count(devices); i++) {
		OBSData ptzcfg = obs_data_array_item(devices, i);
		obs_data_release(ptzcfg);
		ptzDeviceList.make_device(ptzcfg);
	}
}

void ptz_load_devices()
{
	/* Register the proc handlers for issuing PTZ commands */
	proc_handler_t *ph = obs_get_proc_handler();
	if (!ph)
		return;

	/* Preset Recall Callback */
	auto ptz_preset_recall = [](void *data, calldata_t *cd) {
		QMetaObject::invokeMethod(&ptzDeviceList, "preset_recall",
					Q_ARG(uint32_t, calldata_int(cd, "device_id")),
					Q_ARG(int, calldata_int(cd, "preset_id")));
	};
	proc_handler_add(ph, "void ptz_preset_recall(int device_id, int preset_id)",
			ptz_preset_recall, NULL);

	/* Pantilt Callback */
	auto ptz_pantilt = [](void *data, calldata_t *cd) {
		QMetaObject::invokeMethod(&ptzDeviceList, "pantilt",
					Q_ARG(uint32_t, calldata_int(cd, "device_id")),
					Q_ARG(double, calldata_float(cd, "pan")),
					Q_ARG(double, calldata_float(cd, "tilt")));
	};
	proc_handler_add(ph, "void ptz_pantilt(int device_id, float pan, float tilt)",
			ptz_pantilt, NULL);
}
