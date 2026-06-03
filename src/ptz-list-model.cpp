/* Pan Tilt Zoom Controls - Qt list model for managing devices
 *
 * Copyright 2020-2026 Grant Likely <grant.likely@secretlab.ca>
 *
 * SPDX-License-Identifier: GPLv2
 */

#include <obs.hpp>
#include "ptz-list-model.hpp"
#include "ptz-device.hpp"
#include "ptz-visca-udp.hpp"
#include "ptz-visca-tcp.hpp"
#include "ptz-onvif.hpp"
#include "ptz-usb-cam.hpp"
#include "ptz.h"
#include "protocol-helpers.hpp"

#if defined(ENABLE_SERIALPORT)
#include "ptz-visca-uart.hpp"
#include "ptz-pelco.hpp"
#endif

PTZListModel ptzDeviceList;

static void source_rename_cb(void *data, calldata_t *cd)
{
	auto ptzlm = static_cast<PTZListModel *>(data);
	ptzlm->renameDevice(calldata_string(cd, "new_name"), calldata_string(cd, "prev_name"));
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

	auto ptz = getDevice(index);
	if (!ptz)
		return QVariant();

	if (role == Qt::DisplayRole || role == Qt::EditRole)
		return ptz->objectName();

	if (role == PTZListModel::DeviceIdRole)
		return ptz->getId();

	if (role == PTZListModel::DescriptionRole)
		return ptz->description();

	if (role == PTZListModel::IsLiveRole)
		return ptz->isLive();

	if (role == PTZListModel::IsConnectedRole)
		return ptz->isConnected();

	if (role == PTZListModel::IsLockedRole)
		return ptz->isLocked();

	if (role == PTZListModel::SupportsSetHomeRole)
		return ptz->supportsSetHome();

	return QVariant();
}

bool PTZListModel::setData(const QModelIndex &index, const QVariant &value, int role)
{
	auto ptz = getDevice(index);
	if (!ptz)
		return false;

	if (role == PTZListModel::IsLockedRole && ptz->isLocked() != value.toBool()) {
		ptz->setLock(value.toBool());
		emit dataChanged(index, index, {role});
		return true;
	}

	return false;
}

void PTZListModel::do_reset()
{
	beginResetModel();
	endResetModel();
}

void PTZListModel::name_changed(PTZDevice *ptz)
{
	auto index = indexFromDeviceId(ptz->id);
	if (index.isValid())
		emit dataChanged(index, index);
}

void PTZListModel::onSceneChanged()
{
	for (PTZDevice *ptz : devices)
		ptz->onSceneChanged();
}

PTZDevice *PTZListModel::getDevice(const QModelIndex &index) const
{
	if (!index.isValid() || index.model() != this)
		return nullptr;
	return devices.value(index.row());
}

PTZDevice *PTZListModel::getDevice(uint32_t device_id) const
{
	return devicesById.value(device_id, nullptr);
}

PTZDevice *PTZListModel::getDeviceByName(const QString &name) const
{
	for (PTZDevice *ptz : devices)
		if (name == ptz->objectName())
			return ptz;
	return nullptr;
}

QStringList PTZListModel::getDeviceNames() const
{
	QStringList names;
	for (const PTZDevice *ptz : devices)
		names.append(ptz->objectName());
	return names;
}

/**
 * This variant of callDevice accepts a QModelIndex into the data model
 * to choose which device will receive the call
 */
bool PTZListModel::callDevice(const QModelIndex &index, const char *method, calldata_t *cd)
{
	auto ptz = getDevice(index);
	return ptz ? proc_handler_call(ptz->handler, method, cd) : false;
}

/**
 * This variant of callDevice extracts the device_id from calldata
 * before calling the device's proc handler.
 */
bool PTZListModel::callDevice(const char *method, calldata_t *cd)
{
	auto ptz = getDevice(calldata_int(cd, "device_id"));
	return ptz ? proc_handler_call(ptz->handler, method, cd) : false;
}

QModelIndex PTZListModel::indexFromDeviceId(uint32_t device_id)
{
	auto ptz = getDevice(device_id);
	if (ptz)
		return index(devices.indexOf(ptz), 0);
	return QModelIndex();
}

/**
 * Look up model index from the device name
 */
QModelIndex PTZListModel::indexFromName(const QString &name)
{
	for (auto row = 0; row < devices.size(); row++) {
		if (name == devices.at(row)->objectName())
			return index(row, 0);
	}
	return QModelIndex();
}

void PTZListModel::save(OBSDataArray configs) const
{
	for (PTZDevice *ptz : devices) {
		OBSDataAutoRelease cfg = obs_data_create();
		ptz->save(cfg.Get());
		obs_data_array_push_back(configs, cfg);
	}
}

void PTZListModel::add(PTZDevice *ptz)
{
	/* Assign a unique ID */
	uint32_t id = ptz->getId();
	while (devicesById.contains(id) || id == 0)
		id++;
	ptz->id = id;
	devices.append(ptz);
	devicesById[ptz->id] = ptz;
	do_reset();

	connect(ptz, &PTZDevice::settingsChanged, this, &PTZListModel::deviceSettingsChanged);
}

void PTZListModel::removeDevice(const QModelIndex &index)
{
	PTZDevice *ptz = getDevice(index);
	if (ptz) {
		delete ptz;
		disconnect(ptz, nullptr, this, nullptr);
	}
}

void PTZListModel::remove(PTZDevice *ptz)
{
	devicesById.remove(ptz->getId());
	devices.removeAll(ptz);
	do_reset();
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
#if defined(ENABLE_ONVIF)
	if (type == "onvif")
		ptz = new PTZOnvif(config);
#endif /* ENABLE_ONVIF */
#if defined(ENABLE_USB_CAM)
	if (type == "usb-cam")
		ptz = new PTZUSBCam(config);
#endif /* ENABLE_USB_CAM */
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

void PTZListModel::preset_save(uint32_t device_id, int preset_id)
{
	PTZDevice *ptz = getDevice(device_id);
	if (ptz)
		ptz->memory_set(preset_id);
}

void PTZListModel::deviceSettingsChanged(PTZDevice *ptz, OBSData)
{
	int row = devices.indexOf(ptz);
	if (row < 0)
		return;
	auto idx = index(row, 0);
	emit dataChanged(idx, idx);
}
