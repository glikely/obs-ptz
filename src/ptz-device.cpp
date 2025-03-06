/* Pan Tilt Zoom device factory
 *
 * Copyright 2020 Grant Likely <grant.likely@secretlab.ca>
 *
 * SPDX-License-Identifier: GPLv2
 */

#include <obs.hpp>
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

PTZDevice *PTZListModel::getDevice(const QModelIndex &index) const
{
	if (index.row() < 0)
		return nullptr;
	return devices.value(devices.keys().at(index.row()));
}

uint32_t PTZListModel::getDeviceId(const QModelIndex &index) const
{
	if (index.row() < 0)
		return 0;
	return devices.keys().at(index.row());
}

PTZDevice *PTZListModel::getDevice(uint32_t device_id) const
{
	return devices.value(device_id, nullptr);
}

PTZDevice *PTZListModel::getDeviceByName(const QString &name) const
{
	for (auto key : devices.keys()) {
		auto ptz = devices.value(key);
		if (name == ptz->objectName())
			return ptz;
	}
	return NULL;
}

QStringList PTZListModel::getDeviceNames() const
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

bool PTZPresetListModel::insertRows(int row, int count,
				    const QModelIndex &parent)
{
	if (row < 0 || row > rowCount())
		return false;
	size_t curr_id = 0;
	QList<size_t> ids;
	for (int i = 0; i < count; i++) {
		while (m_presets.contains(curr_id))
			curr_id++;
		if (curr_id >= m_maxPresets)
			return false;
		ids.append(curr_id);
	}

	beginInsertRows(parent, row, count);
	for (auto id : ids) {
		QVariantMap map;
		map["id"] = (uint)id;
		m_presets[id] = map;
		m_displayOrder.insert(row++, id);
	}
	endInsertRows();
	return true;
}

bool PTZPresetListModel::removeRows(int row, int count,
				    const QModelIndex &parent)
{
	if (row < 0 || row >= rowCount())
		return false;
	beginRemoveRows(parent, row, count);
	QList<size_t> ids = m_displayOrder.mid(row, count);
	;
	for (auto id : ids) {
		m_displayOrder.removeAt(row);
		m_presets.remove(id);
	}
	endRemoveRows();
	return true;
}

bool PTZPresetListModel::moveRows(const QModelIndex &srcParent, int srcRow,
				  int count, const QModelIndex &destParent,
				  int destChild)
{
	if (srcRow < 0 || srcRow >= rowCount() || destChild < 0 ||
	    destChild > rowCount() || count != 1)
		return false;

	if (!beginMoveRows(srcParent, srcRow, srcRow + count - 1, destParent,
			   destChild))
		return false;
	if (srcRow < destChild)
		destChild--;
	m_displayOrder.move(srcRow, destChild);
	endMoveRows();
	return true;
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
	if (type == "usb-cam")
        ptz = new PTZUSBCam(config);
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
	PTZDevice *ptz = ptzDeviceList.getDevice(device_id);
	if (ptz)
		ptz->memory_set(preset_id);
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

int PTZPresetListModel::rowCount(const QModelIndex &parent) const
{
	Q_UNUSED(parent);
	return (int)m_displayOrder.size();
}

int PTZPresetListModel::getPresetId(const QModelIndex &index) const
{
	if (!index.isValid())
		return -1;

	if (index.row() >= m_displayOrder.size()) {
		blog(LOG_ERROR, "ERROR: Preset Row %i is not valid",
		     index.row());
		return -1;
	}
	return (int)m_displayOrder[index.row()];
}

QVariant PTZPresetListModel::data(const QModelIndex &index, int role) const
{
	auto id = getPresetId(index);
	if (id < 0)
		return QVariant();
	auto preset = m_presets[id];
	QString name = preset["name"].toString();
	if (role == Qt::DisplayRole) {
		if (name == "")
			name = QString("Preset %1").arg(id + 1);
		return name + QString(" (%1)").arg(id);
	}
	if (role == Qt::EditRole)
		return name;
	if (role == Qt::UserRole)
		return id;

	return QVariant();
}

Qt::ItemFlags PTZPresetListModel::flags(const QModelIndex &index) const
{
	auto f = QAbstractListModel::flags(index);
	if (index.column() == 0)
		f |= Qt::ItemIsEditable;
	return f;
}

void PTZPresetListModel::sanitize(size_t id)
{
	if (!m_presets.contains(id))
		return;
	if (!m_displayOrder.contains(id))
		m_displayOrder.append(id);
	QVariantMap &preset = m_presets[id];
	QString name = preset["name"].toString();
	if (name == "" || name == QString("Preset %1").arg(id))
		preset.remove("name");
}

bool PTZPresetListModel::setData(const QModelIndex &index,
				 const QVariant &value, int role)
{
	auto id = getPresetId(index);
	if (id < 0)
		return false;

	if (role == Qt::EditRole) {
		QVariantMap &preset = m_presets[id];
		preset["name"] = value.toString();
		sanitize(id);
		emit dataChanged(index, index);
		return true;
	}
	return false;
}

/* Insert a new preset and return the ID */
int PTZPresetListModel::newPreset()
{
	size_t id = 0;
	while (m_presets.contains(id))
		id++;
	if (id >= m_maxPresets)
		return -1;

	QVariantMap map;
	map["id"] = (uint)id;
	beginInsertRows(QModelIndex(), rowCount(), 1);
	m_presets[id] = map;
	m_displayOrder.append(id);
	endInsertRows();
	return (int)id;
}

QVariant PTZPresetListModel::presetProperty(size_t id, QString key)
{
	/* Safe to derefernce unconditionally here. Both levels will return
	 * default empty values without segfaulting */
	return m_presets[id][key];
}

bool PTZPresetListModel::updatePreset(size_t id, const QVariantMap &map)
{
	if (!m_presets.contains(id))
		return false;
	m_presets[id].insert(map);
	return true;
}

int PTZPresetListModel::find(QString key, QVariant value)
{
	/* Search for the matching key/value pair in all presets.
	 * This is an O(N) operation */
	auto end = m_presets.cend();
	for (auto i = m_presets.cbegin(); i != end; i++) {
		if (i.value()[key] == value)
			return (int)i.key();
	}
	return -1;
}

void PTZPresetListModel::loadPresets(OBSDataArray preset_array)
{
	if (!preset_array)
		return;
	beginResetModel();
	m_presets.clear();
	m_displayOrder.clear();
	for (size_t i = 0; i < obs_data_array_count(preset_array); i++) {
		OBSDataAutoRelease item = obs_data_array_item(preset_array, i);
		auto id = obs_data_get_int(item, "id");
		if (m_displayOrder.contains(id))
			continue;
		QVariantMap preset = OBSDataToVariantMap(item.Get());
		m_presets[id] = preset;
		sanitize(id);
	}
	endResetModel();
}

OBSDataArray PTZPresetListModel::savePresets() const
{
	OBSDataArrayAutoRelease preset_array = obs_data_array_create();
	for (auto id : m_displayOrder) {
		OBSDataAutoRelease data = variantMapToOBSData(m_presets[id]);
		obs_data_set_int(data, "id", id);
		obs_data_array_push_back(preset_array, data);
	}
	return preset_array.Get();
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
}

PTZDevice::~PTZDevice()
{
	ptzDeviceList.remove(this);
}

void PTZDevice::setObjectName(QString name)
{
	name = name.simplified();
	if (name == "") {
		if (objectName().startsWith("PTZ Device"))
			return;
		name = "PTZ Device";
	}
	if (name == objectName())
		return;
	QString new_name = name;
	for (int i = 1;; i++) {
		PTZDevice *ptz = ptzDeviceList.getDeviceByName(new_name);
		if (!ptz)
			break;
		new_name = name + " " + QString::number(i);
	}
	QObject::setObjectName(new_name);
	ptzDeviceList.name_changed(this);
}

QString PTZDevice::description()
{
	return QString::fromStdString(type);
}

void PTZDevice::pantilt(double pan, double tilt)
{
	pan = std::clamp(pan, -pantilt_speed_max, pantilt_speed_max);
	tilt = std::clamp(tilt, -pantilt_speed_max, pantilt_speed_max);
	if ((pan_speed == pan) && (tilt_speed == tilt))
		return;
	pan_speed = pan;
	tilt_speed = tilt;
	status |= STATUS_PANTILT_SPEED_CHANGED;
	do_update();
}

void PTZDevice::zoom(double speed)
{
	speed = std::clamp(speed, -zoom_speed_max, zoom_speed_max);
	if (zoom_speed == speed)
		return;
	zoom_speed = speed;
	status |= STATUS_ZOOM_SPEED_CHANGED;
	do_update();
}

void PTZDevice::focus(double speed)
{
	speed = std::clamp(speed, -focus_speed_max, focus_speed_max);
	if (focus_speed == speed)
		return;
	focus_speed = speed;
	status |= STATUS_FOCUS_SPEED_CHANGED;
	do_update();
}

void PTZDevice::set_config(OBSData config)
{
	/* Update the list of preset names */
	obs_data_set_default_double(config, "preset_max", 16);
	m_presetsModel.setMaxPresets(
		(size_t)obs_data_get_int(config, "preset_max"));
	OBSDataArrayAutoRelease preset_array =
		obs_data_get_array(config, "presets");
	m_presetsModel.loadPresets(preset_array.Get());

	obs_data_set_default_double(config, "pantilt_speed_max", 1.0);
	obs_data_set_default_double(config, "zoom_speed_max", 1.0);
	obs_data_set_default_double(config, "focus_speed_max", 1.0);
	pantilt_speed_max = obs_data_get_double(config, "pantilt_speed_max");
	zoom_speed_max = obs_data_get_double(config, "zoom_speed_max");
	focus_speed_max = obs_data_get_double(config, "focus_speed_max");
}

OBSData PTZDevice::get_config()
{
	OBSData config = obs_data_create();
	obs_data_release(config);

	obs_data_set_string(config, "name", QT_TO_UTF8(objectName()));
	obs_data_set_int(config, "id", id);
	obs_data_set_string(config, "type", type.c_str());
	obs_data_set_double(config, "pantilt_speed_max", pantilt_speed_max);
	obs_data_set_double(config, "zoom_speed_max", zoom_speed_max);
	obs_data_set_double(config, "focus_speed_max", focus_speed_max);
	obs_data_set_int(config, "preset_max", m_presetsModel.maxPresets());

	OBSDataArrayAutoRelease preset_array = m_presetsModel.savePresets();
	obs_data_set_array(config, "presets", preset_array);
	return config;
}

void PTZDevice::set_settings(OBSData config)
{
	if (obs_data_has_user_value(config, "name"))
		setObjectName(obs_data_get_string(config, "name"));
	if (obs_data_has_user_value(config, "pantilt_speed_max"))
		pantilt_speed_max =
			obs_data_get_double(config, "pantilt_speed_max");
	if (obs_data_has_user_value(config, "zoom_speed_max"))
		zoom_speed_max = obs_data_get_double(config, "zoom_speed_max");
	if (obs_data_has_user_value(config, "focus_speed_max"))
		focus_speed_max =
			obs_data_get_double(config, "focus_speed_max");
	if (obs_data_has_user_value(config, "preset_max"))
		m_presetsModel.setMaxPresets(
			(int)obs_data_get_int(config, "preset_max"));
}

OBSData PTZDevice::get_settings()
{
	obs_data_apply(settings, get_config());
	return settings;
}

obs_properties_t *PTZDevice::get_obs_properties()
{
	obs_properties_t *rtn_props = obs_properties_create();

	/* Combo box list for associated OBS source */
	auto src_cb = [](void *data, obs_source_t *src) {
		auto srcnames = static_cast<QStringList *>(data);
		if (obs_source_get_type(src) != OBS_SOURCE_TYPE_SCENE)
			srcnames->append(obs_source_get_name(src));
		return true;
	};
	auto srcs_prop = obs_properties_add_list(rtn_props, "name", "Source",
						 OBS_COMBO_TYPE_LIST,
						 OBS_COMBO_FORMAT_STRING);
	obs_property_list_add_string(srcs_prop, "---select source---", "");
	/* Add current source to top list */
	OBSSourceAutoRelease src =
		obs_get_source_by_name(QT_TO_UTF8(objectName()));
	if (src)
		obs_property_list_add_string(srcs_prop,
					     QT_TO_UTF8(objectName()),
					     QT_TO_UTF8(objectName()));
	/* Add all sources not assigned to a camera */
	QStringList srcnames;
	obs_enum_sources(src_cb, &srcnames);
	for (auto n : ptzDeviceList.getDeviceNames())
		srcnames.removeAll(n);
	for (auto n : srcnames)
		obs_property_list_add_string(srcs_prop, QT_TO_UTF8(n),
					     QT_TO_UTF8(n));

	obs_properties_t *config = obs_properties_create();
	obs_properties_add_group(rtn_props, "interface", "Connection",
				 OBS_GROUP_NORMAL, config);

	/* Generic camera limits and speeds properties */
	auto speed = obs_properties_create();
	obs_properties_add_group(rtn_props, "general", "Camera Settings",
				 OBS_GROUP_NORMAL, speed);
	obs_properties_add_int_slider(speed, "preset_max", "Number of presets",
				      1, 0x80, 1);
	obs_properties_add_float_slider(speed, "pantilt_speed_max",
					"Pan/Tilt Maximum Speed", 0.1, 1.0,
					1.0 / 1024);
	obs_properties_add_float_slider(speed, "zoom_speed_max",
					"Zoom Maximum Speed", 0.1, 1.0,
					1.0 / 1024);
	obs_properties_add_float_slider(speed, "focus_speed_max",
					"Focus Maximum Speed", 0.1, 1.0,
					1.0 / 1024);

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

	/* Preset Recall/Save Callback */
	auto ptz_preset_cb = [](void *data, calldata_t *cd) {
		auto function = static_cast<const char *>(data);
		QMetaObject::invokeMethod(
			&ptzDeviceList, function,
			Q_ARG(uint32_t, calldata_int(cd, "device_id")),
			Q_ARG(int, calldata_int(cd, "preset_id")));
	};
	proc_handler_add(ptz_ph,
			 "void ptz_preset_recall(int device_id, int preset_id)",
			 ptz_preset_cb, (void *)"preset_recall");
	proc_handler_add(ptz_ph,
			 "void ptz_preset_save(int device_id, int preset_id)",
			 ptz_preset_cb, (void *)"preset_save");

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
