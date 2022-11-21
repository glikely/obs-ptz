/* Pan Tilt Zoom device factory
 *
 * Copyright 2020 Grant Likely <grant.likely@secretlab.ca>
 *
 * SPDX-License-Identifier: GPLv2
 */

#include <obs.hpp>
#include "imported/qt-wrappers.hpp"
#include "ptz-device.hpp"
#include "ptz-visca-udp.hpp"
#include "ptz-visca-tcp.hpp"
#include "ptz-onvif.hpp"
#include "ptz.h"

#if defined(ENABLE_SERIALPORT)
#include "ptz-visca-uart.hpp"
#include "ptz-pelco.hpp"
#endif

int ptz_debug_level = LOG_DEBUG;

PTZListModel ptzDeviceList;
QMap<uint32_t, PTZDevice *> PTZListModel::devices;

static void source_rename_cb(void *data, calldata_t *cd)
{
	auto ptzlm = static_cast<PTZListModel *>(data);
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
	//signal_handler_t *sh = obs_get_signal_handler();
	//signal_handler_disconnect(sh, "source_rename", source_rename_cb, this);
}

int PTZListModel::rowCount(const QModelIndex &parent) const
{
	Q_UNUSED(parent);
	return (int)devices.size();
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
		return devices.value(devices.keys().at(index.row()))
			->objectName();
	}
#if 0
	if (role == Qt::UserRole) {
		return get_device(index.row());
	}
#endif
	return QVariant();
}

bool PTZListModel::setData(const QModelIndex &index, const QVariant &value,
			   int role)
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
	return devices.value(devices.keys().at(index.row()));
}

uint32_t PTZListModel::getDeviceId(const QModelIndex &index)
{
	if (index.row() < 0)
		return 0;
	return devices.keys().at(index.row());
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
	int row = (int)devices.keys().indexOf(device_id);
	if (row < 0)
		return QModelIndex();
	return index(row, 0);
}

obs_data_array_t *PTZListModel::getConfigs()
{
	obs_data_array_t *configs = obs_data_array_create();
	for (auto key : devices.keys())
		obs_data_array_push_back(
			configs, ptzDeviceList.getDevice(key)->get_config());
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

#if defined(ENABLE_SERIALPORT)
	if (type == "pelco" || type == "pelco-p")
		ptz = new PTZPelco(config);
	if (type == "visca")
		ptz = new PTZViscaSerial(config);
#endif /* ENABLE_SERIALPORT */
	if (type == "visca-over-ip")
		ptz = new PTZViscaOverIP(config);
	if (type == "visca-over-tcp")
		ptz = new PTZViscaOverTCP(config);
	if (type == "onvif")
		ptz = new PTZOnvif(config);
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
	PTZDevice *ptz = ptzDeviceList.getDevice(device_id);
	if (ptz)
		ptz->memory_recall(preset_id);
}

enum {
	MOVE_FLAG_PANTILT = 1 << 0,
	MOVE_FLAG_ZOOM = 1 << 1,
	MOVE_FLAG_FOCUS = 1 << 2,
};

void PTZListModel::move_continuous(uint32_t device_id, uint32_t flags,
				   double pan, double tilt, double zoom,
				   double focus)
{
	PTZDevice *ptz = ptzDeviceList.getDevice(device_id);
	if (!ptz)
		return;

	if (flags & MOVE_FLAG_PANTILT)
		ptz->pantilt(pan, tilt);
	if (flags & MOVE_FLAG_ZOOM)
		ptz->zoom(zoom);
	if (flags & MOVE_FLAG_FOCUS)
		ptz->focus(focus);
}

PTZDevice::PTZDevice(OBSData config) : QObject()
{
	setObjectName(obs_data_get_string(config, "name"));
	id = (int)obs_data_get_int(config, "id");
	type = obs_data_get_string(config, "type");
	settings = obs_data_create();
	obs_data_release(settings);
	stale_settings = {"pan_pos", "tilt_pos", "zoom_pos", "focus_pos"};
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
		for (size_t i = 0; i < obs_data_array_count(preset_array);
		     i++) {
			OBSData preset = obs_data_array_item(preset_array, i);
			obs_data_release(preset);
			int preset_id = (int)obs_data_get_int(preset, "id");
			const char *preset_name =
				obs_data_get_string(preset, "name");
			if ((preset_id >= 0) &&
			    (preset_id < preset_names.size()) && preset_name)
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
	obs_properties_t *rtn_props = obs_properties_create();
	obs_properties_t *config = obs_properties_create();
	obs_properties_add_group(rtn_props, "interface", "Connection",
				 OBS_GROUP_NORMAL, config);

	return rtn_props;
}

/* C interface for non-QT parts of the plugin */
obs_data_array_t *ptz_devices_get_config()
{
	return ptzDeviceList.getConfigs();
}

obs_source_t *ptz_device_find_source_using_ptz_name(uint32_t device_id)
{
	PTZDevice *ptz = ptzDeviceList.getDevice(device_id);
	if (!ptz)
		return NULL;
	return obs_get_source_by_name(QT_TO_UTF8(ptz->objectName()));
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

static proc_handler_t *ptz_ph = NULL;

proc_handler_t *ptz_get_proc_handler()
{
	return ptz_ph;
}

void ptz_load_devices()
{
	/* Register the proc handlers for issuing PTZ commands */
	ptz_ph = proc_handler_create();
	if (!ptz_ph)
		return;

	/* Preset Recall Callback */
	auto ptz_preset_recall = [](void *data, calldata_t *cd) {
		Q_UNUSED(data);
		QMetaObject::invokeMethod(
			&ptzDeviceList, "preset_recall",
			Q_ARG(uint32_t, calldata_int(cd, "device_id")),
			Q_ARG(int, calldata_int(cd, "preset_id")));
	};
	proc_handler_add(ptz_ph,
			 "void ptz_preset_recall(int device_id, int preset_id)",
			 ptz_preset_recall, NULL);

	auto ptz_move_continuous = [](void *data, calldata_t *cd) {
		Q_UNUSED(data);
		long long device_id;
		double pan, tilt, zoom, focus;
		if (!calldata_get_int(cd, "device_id", &device_id))
			return;
		int flags = 0;
		if (calldata_get_float(cd, "pan", &pan) &&
		    calldata_get_float(cd, "tilt", &tilt))
			flags |= MOVE_FLAG_PANTILT;
		if (calldata_get_float(cd, "zoom", &zoom))
			flags |= MOVE_FLAG_ZOOM;
		if (calldata_get_float(cd, "focus", &focus))
			flags |= MOVE_FLAG_FOCUS;
		QMetaObject::invokeMethod(
			&ptzDeviceList, "move_continuous",
			Q_ARG(uint32_t, device_id), Q_ARG(uint32_t, flags),
			Q_ARG(double, pan), Q_ARG(double, tilt),
			Q_ARG(double, zoom), Q_ARG(double, focus));
	};
	proc_handler_add(
		ptz_ph,
		"void ptz_move_continuous(int device_id, float pan, float tilt, float zoom, float focus)",
		ptz_move_continuous, NULL);

	/* Register the new proc hander with the main proc handler */
	proc_handler_t *ph = obs_get_proc_handler();
	if (!ph)
		return;

	/* Register a function for retrieving the PTZ call handler */
	auto ptz_get_proc_handler = [](void *data, calldata_t *cd) {
		Q_UNUSED(data);
		calldata_set_ptr(cd, "return", ptz_ph);
	};
	proc_handler_add(ph, "ptr ptz_get_proc_handler()", ptz_get_proc_handler,
			 NULL);

	/* Deprecated pantilt callback for compatibility with existing plugins */
	proc_handler_add(
		ph,
		"void ptz_pantilt(int device_id, float pan, float tilt, float zoom, float focus)",
		ptz_move_continuous, NULL);
}

void ptz_unload_devices(void)
{
	proc_handler_destroy(ptz_ph);
	ptz_ph = nullptr;
}
