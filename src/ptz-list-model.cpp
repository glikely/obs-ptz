/* Pan Tilt Zoom Controls - Qt list model for managing devices
 *
 * Copyright 2020-2026 Grant Likely <grant.likely@secretlab.ca>
 *
 * SPDX-License-Identifier: GPLv2
 */

#include <obs.hpp>
#include <qt-wrappers.hpp>
#include "ptz-list-model.hpp"
#include "ptz.h"
#include "protocol-helpers.hpp"

PTZListModel *ptzDeviceList = nullptr;

static void source_rename_cb(void *data, calldata_t *cd)
{
	auto ptzlm = static_cast<PTZListModel *>(data);
	ptzlm->renameDevice(calldata_string(cd, "new_name"), calldata_string(cd, "prev_name"));
}

/**
 * Device lifetime is detected through the global PTZ signal_handler
 * (see ptz_get_signal_handler() / ptz_load_devices())
 */
static void device_create_cb(void *data, calldata_t *cd)
{
	auto ptzlm = static_cast<PTZListModel *>(data);
	auto ph = static_cast<proc_handler_t *>(calldata_ptr(cd, "proc_handler"));
	auto sh = static_cast<signal_handler_t *>(calldata_ptr(cd, "signal_handler"));
	if (ph && sh)
		ptzlm->deviceCreated((uint32_t)calldata_int(cd, "device_id"), ph, sh);
}

static void device_destroy_cb(void *data, calldata_t *cd)
{
	static_cast<PTZListModel *>(data)->deviceDestroyed((uint32_t)calldata_int(cd, "device_id"));
}

/**
 * Per-device change notification -- connected to PTZDevice's single
 * "state_changed" signal (see PTZListModel::add()). Status, rename, and
 * settings changes alike all fire it: it doesn't carry enough of a payload
 * on its own to selectively patch the cached state PTZListModel keeps for
 * this device (see PTZListModel::deviceStatusChanged()), and doesn't need
 * to -- a full ptz_get_state() re-query is cheap for a UI list this size.
 */
static void device_state_changed_cb(void *data, calldata_t *cd)
{
	static_cast<PTZListModel *>(data)->deviceStatusChanged((uint32_t)calldata_int(cd, "device_id"));
}

/**
 * Preset list mutation bracketing. PTZDevice fires the "before" signal
 * synchronously ahead of actually mutating its preset list, and the
 * "after" signal once the mutation is done -- these trampolines translate
 * that into the begin.../end... pairs QAbstractItemModel requires.
 */
static void preset_insert_cb(void *data, calldata_t *cd)
{
	auto ptzlm = static_cast<PTZListModel *>(data);
	ptzlm->presetBeginInsert((uint32_t)calldata_int(cd, "device_id"), (int)calldata_int(cd, "row"));
}

static void preset_inserted_cb(void *data, calldata_t *cd)
{
	static_cast<PTZListModel *>(data)->presetEndInsert((uint32_t)calldata_int(cd, "device_id"));
}

static void preset_remove_cb(void *data, calldata_t *cd)
{
	auto ptzlm = static_cast<PTZListModel *>(data);
	ptzlm->presetBeginRemove((uint32_t)calldata_int(cd, "device_id"), (int)calldata_int(cd, "row"));
}

static void preset_removed_cb(void *data, calldata_t *cd)
{
	static_cast<PTZListModel *>(data)->presetEndRemove((uint32_t)calldata_int(cd, "device_id"));
}

static void preset_move_cb(void *data, calldata_t *cd)
{
	auto ptzlm = static_cast<PTZListModel *>(data);
	bool ok = ptzlm->presetBeginMove((uint32_t)calldata_int(cd, "device_id"), (int)calldata_int(cd, "src_row"),
					  (int)calldata_int(cd, "dest_row"));
	calldata_set_bool(cd, "return", ok);
}

static void preset_moved_cb(void *data, calldata_t *cd)
{
	static_cast<PTZListModel *>(data)->presetEndMove((uint32_t)calldata_int(cd, "device_id"));
}

static void preset_renamed_cb(void *data, calldata_t *cd)
{
	static_cast<PTZListModel *>(data)->presetsChanged((uint32_t)calldata_int(cd, "device_id"));
}

PTZListModel::PTZListModel() : QAbstractItemModel()
{
	signal_handler_t *sh = obs_get_signal_handler();
	signal_handler_connect(sh, "source_rename", source_rename_cb, this);

	/* Safe to connect directly, unlike a plain static-storage global's
	 * constructor would be: create() runs from ptz_load_devices(), after
	 * the PTZ signal_handler below already exists. */
	signal_handler_t *ptz_sh = ptz_get_signal_handler();
	signal_handler_connect(ptz_sh, "ptz_device_create", device_create_cb, this);
	signal_handler_connect(ptz_sh, "ptz_device_destroy", device_destroy_cb, this);
}

PTZListModel::~PTZListModel()
{
	//signal_handler_t *sh = obs_get_signal_handler();
	//signal_handler_disconnect(sh, "source_rename", source_rename_cb, this);
}

void PTZListModel::create()
{
	if (!ptzDeviceList)
		ptzDeviceList = new PTZListModel();
}

void PTZListModel::destroy()
{
	delete ptzDeviceList;
	ptzDeviceList = nullptr;
}

void PTZListModel::rebuildRowIndex()
{
	rowByDeviceId.clear();
	for (int i = 0; i < devices.size(); i++)
		rowByDeviceId[devices[i].id] = i;
}

PTZListModel::PTZDeviceEntry *PTZListModel::entryAt(int row)
{
	return (row >= 0 && row < devices.size()) ? &devices[row] : nullptr;
}

const PTZListModel::PTZDeviceEntry *PTZListModel::entryAt(int row) const
{
	return (row >= 0 && row < devices.size()) ? &devices[row] : nullptr;
}

/* Only top-level (device) rows carry an entry -- preset rows are tagged
 * with their parent device's id in internalId(), device rows with 0 (never
 * a valid device_id, see PTZDevice::PTZDevice()'s id assignment loop). */
PTZListModel::PTZDeviceEntry *PTZListModel::entryAt(const QModelIndex &index)
{
	if (!checkIndex(index) || index.internalId() != 0)
		return nullptr;
	return entryAt(index.row());
}

const PTZListModel::PTZDeviceEntry *PTZListModel::entryAt(const QModelIndex &index) const
{
	if (!checkIndex(index) || index.internalId() != 0)
		return nullptr;
	return entryAt(index.row());
}

PTZListModel::PTZDeviceEntry *PTZListModel::entryById(uint32_t device_id)
{
	int row = rowByDeviceId.value(device_id, -1);
	return row >= 0 ? &devices[row] : nullptr;
}

const PTZListModel::PTZDeviceEntry *PTZListModel::entryById(uint32_t device_id) const
{
	int row = rowByDeviceId.value(device_id, -1);
	return row >= 0 ? &devices[row] : nullptr;
}

/**
 * Re-fetches everything data() needs to display a device row, via a single
 * ptz_get_state() proc_handler call. Called once to seed a new row, and
 * again whenever a state_changed signal says the cache may be stale.
 */
void PTZListModel::refreshDeviceState(PTZDeviceEntry *entry)
{
	calldata_t cd = {};
	proc_handler_call(entry->ph, "ptz_get_state", &cd);
	auto state = static_cast<obs_data_t *>(calldata_ptr(&cd, "return"));
	if (state) {
		entry->name = QT_UTF8(obs_data_get_string(state, "name"));
		entry->description = QT_UTF8(obs_data_get_string(state, "description"));
		entry->connected = obs_data_get_bool(state, "connected");
		entry->live = obs_data_get_bool(state, "live");
		entry->preview = obs_data_get_bool(state, "preview");
		entry->locked = obs_data_get_bool(state, "locked");
		entry->supportsSetHome = obs_data_get_bool(state, "supports_set_home");
		entry->maxPresets = (int)obs_data_get_int(state, "max_presets");
		obs_data_release(state);
	}
	calldata_free(&cd);
}

/**
 * Re-fetches a device's preset list via ptz_preset_get_list(). Must be
 * called *before* the matching endInsertRows()/endRemoveRows()/
 * endMoveRows() -- QAbstractItemModel requires the data to already reflect
 * the new row layout by the time the end*() call returns.
 */
void PTZListModel::refreshPresetList(PTZDeviceEntry *entry)
{
	entry->presets.clear();
	calldata_t cd = {};
	proc_handler_call(entry->ph, "ptz_preset_get_list", &cd);
	auto list = static_cast<obs_data_array_t *>(calldata_ptr(&cd, "return"));
	if (list) {
		for (size_t i = 0; i < obs_data_array_count(list); i++) {
			OBSDataAutoRelease item = obs_data_array_item(list, i);
			PresetEntry preset;
			preset.id = (int)obs_data_get_int(item, "id");
			preset.name = QT_UTF8(obs_data_get_string(item, "name"));
			preset.token = QT_UTF8(obs_data_get_string(item, "token"));
			entry->presets.append(preset);
		}
		obs_data_array_release(list);
	}
	calldata_free(&cd);
}

QModelIndex PTZListModel::index(int row, int column, const QModelIndex &parent) const
{
	if (!checkIndex(parent) || row < 0 || column != 0)
		return QModelIndex();

	/* The parent == root, then this is a device index */
	if (!parent.isValid())
		return createIndex(row, column, (quintptr)0);

	/* Only device rows (internalId() == 0) can parent presets */
	if (parent.internalId() != 0)
		return QModelIndex();
	auto entry = entryAt(parent.row());
	if (!entry)
		return QModelIndex();
	return createIndex(row, column, (quintptr)entry->id);
}

QModelIndex PTZListModel::parent(const QModelIndex &child) const
{
	quintptr device_id = child.internalId();
	if (device_id != 0) {
		int row = rowByDeviceId.value((uint32_t)device_id, -1);
		if (row >= 0)
			return createIndex(row, 0, (quintptr)0);
	}

	return QModelIndex();
}

int PTZListModel::rowCount(const QModelIndex &parent) const
{
	if (!checkIndex(parent))
		return 0;

	/* If parent == root, then this is a device index */
	if (!parent.isValid())
		return devices.size();

	if (parent.internalId() != 0)
		return 0;
	auto entry = entryAt(parent.row());
	return entry ? entry->presets.size() : 0;
}

Qt::ItemFlags PTZListModel::flags(const QModelIndex &index) const
{
	if (!index.isValid())
		return Qt::ItemIsEnabled;
	return QAbstractItemModel::flags(index) | Qt::ItemIsEditable;
}

bool PTZListModel::insertRows(int row, int count, const QModelIndex &parent)
{
	auto entry = entryAt(parent);
	if (!entry)
		return false;
	if (row < 0 || count <= 0 || row > entry->presets.size() || entry->presets.size() + count > entry->maxPresets)
		return false;

	for (int i = 0; i < count; i++) {
		calldata_t cd = {};
		calldata_set_int(&cd, "device_id", entry->id);
		calldata_set_int(&cd, "row", row++);
		proc_handler_call(entry->ph, "ptz_preset_new", &cd);
		calldata_free(&cd);
	}
	return true;
}

bool PTZListModel::removeRows(int row, int count, const QModelIndex &parent)
{
	auto entry = entryAt(parent);
	if (!entry)
		return false;
	if (row < 0 || count <= 0 || row + count - 1 >= entry->presets.size())
		return false;

	for (int i = 0; i < count; i++) {
		calldata_t cd = {};
		calldata_set_int(&cd, "device_id", entry->id);
		calldata_set_int(&cd, "row", row);
		proc_handler_call(entry->ph, "ptz_preset_remove", &cd);
		calldata_free(&cd);
	}
	return true;
}

bool PTZListModel::moveRows(const QModelIndex &srcParent, int srcRow, int count, const QModelIndex &destParent,
			    int destChild)
{
	if (!checkIndex(srcParent) || srcParent != destParent)
		return false;
	auto entry = entryAt(srcParent);
	if (!entry)
		return false;
	if (srcRow < 0 || srcRow + count - 1 >= rowCount(srcParent))
		return false;
	if (destChild < 0 || destChild > rowCount(destParent))
		return false;
	if (count != 1)
		return false;

	calldata_t cd = {};
	calldata_set_int(&cd, "device_id", entry->id);
	calldata_set_int(&cd, "src_row", srcRow);
	calldata_set_int(&cd, "dest_row", destChild);
	proc_handler_call(entry->ph, "ptz_preset_move", &cd);
	calldata_free(&cd);
	return true;
}

QVariant PTZListModel::data(const QModelIndex &index, int role) const
{
	if (!index.isValid())
		return QVariant();

	if (index.internalId() != 0) {
		/* Preset row -- internalId() is the parent device's id */
		auto entry = entryById((uint32_t)index.internalId());
		if (!entry || index.row() < 0 || index.row() >= entry->presets.size())
			return QVariant();
		const auto &preset = entry->presets.at(index.row());

		if (role == Qt::DisplayRole) {
			if (!preset.name.isEmpty())
				return preset.name;
			return QString(obs_module_text("PTZ.PresetNum")).arg(preset.id);
		}
		if (role == Qt::ToolTipRole) {
			if (!preset.token.isEmpty())
				return QString(obs_module_text("PTZ.Preset.Tooltip")).arg("'" + preset.token + "'");
			return QString(obs_module_text("PTZ.Preset.Tooltip")).arg(preset.id);
		}
		if (role == Qt::EditRole)
			return preset.name;
		if (role == Qt::UserRole)
			return preset.id;
		if (role == Qt::SizeHintRole)
			return QSize(0, 20);

		return QVariant();
	}

	auto entry = entryAt(index.row());
	if (!entry)
		return QVariant();

	if (role == Qt::DisplayRole || role == Qt::EditRole)
		return entry->name;

	if (role == PTZListModel::DeviceIdRole)
		return entry->id;

	if (role == PTZListModel::DescriptionRole)
		return entry->description;

	if (role == PTZListModel::IsLiveRole)
		return entry->live;

	if (role == PTZListModel::IsPreviewRole)
		return entry->preview;

	if (role == PTZListModel::IsConnectedRole)
		return entry->connected;

	if (role == PTZListModel::IsLockedRole)
		return entry->locked;

	if (role == PTZListModel::SupportsSetHomeRole)
		return entry->supportsSetHome;

	return QVariant();
}

bool PTZListModel::setData(const QModelIndex &index, const QVariant &value, int role)
{
	if (index.internalId() != 0) {
		auto entry = entryById((uint32_t)index.internalId());
		if (!entry || index.row() < 0 || index.row() >= entry->presets.size())
			return false;

		if (role == Qt::EditRole) {
			calldata_t cd = {};
			calldata_set_int(&cd, "device_id", entry->id);
			calldata_set_int(&cd, "id", entry->presets.at(index.row()).id);
			calldata_set_string(&cd, "name", QT_TO_UTF8(value.toString()));
			proc_handler_call(entry->ph, "ptz_preset_set_name", &cd);
			calldata_free(&cd);
			/* cache refresh + dataChanged happen synchronously inside
			 * that call, via the preset_renamed signal */
			return true;
		}
		return false;
	}

	auto entry = entryAt(index.row());
	if (!entry)
		return false;

	if (role == PTZListModel::IsLockedRole && entry->locked != value.toBool()) {
		calldata_t cd = {};
		calldata_set_int(&cd, "device_id", entry->id);
		calldata_set_bool(&cd, "locked", value.toBool());
		proc_handler_call(entry->ph, "ptz_set_locked", &cd);
		calldata_free(&cd);
		/* cache refresh + dataChanged happen synchronously inside that
		 * call, via the state_changed signal */
		return true;
	}

	return false;
}

void PTZListModel::do_reset()
{
	beginResetModel();
	endResetModel();
}

void PTZListModel::onSceneChanged()
{
	calldata_t cd = {};
	for (const auto &entry : devices)
		proc_handler_call(entry.ph, "ptz_scene_changed", &cd);
	calldata_free(&cd);
}

/**
 * This variant of callDevice accepts a QModelIndex into the data model
 * to choose which device will receive the call
 */
bool PTZListModel::callDevice(const QModelIndex &index, const char *method, calldata_t *cd)
{
	auto entry = entryAt(index);
	return entry ? proc_handler_call(entry->ph, method, cd) : false;
}

/**
 * This variant of callDevice extracts the device_id from calldata
 * before calling the device's proc handler.
 */
bool PTZListModel::callDevice(const char *method, calldata_t *cd)
{
	auto entry = entryById((uint32_t)calldata_int(cd, "device_id"));
	return entry ? proc_handler_call(entry->ph, method, cd) : false;
}

QModelIndex PTZListModel::indexFromDeviceId(uint32_t device_id) const
{
	int row = rowByDeviceId.value(device_id, -1);
	return row >= 0 ? index(row, 0) : QModelIndex();
}

/**
 * Look up model index from the device name
 */
QModelIndex PTZListModel::indexFromName(const QString &name) const
{
	for (int row = 0; row < devices.size(); row++)
		if (name == devices.at(row).name)
			return index(row, 0);
	return QModelIndex();
}

void PTZListModel::renameDevice(QString new_name, QString prev_name)
{
	for (const auto &entry : devices) {
		if (entry.name != prev_name)
			continue;
		calldata_t cd = {};
		calldata_set_int(&cd, "device_id", entry.id);
		calldata_set_string(&cd, "name", QT_TO_UTF8(new_name));
		proc_handler_call(entry.ph, "ptz_set_name", &cd);
		calldata_free(&cd);
		return;
	}
}

void PTZListModel::save(OBSDataArray configs) const
{
	for (const auto &entry : devices) {
		OBSDataAutoRelease cfg = obs_data_create();
		calldata_t cd = {};
		calldata_set_int(&cd, "device_id", entry.id);
		calldata_set_ptr(&cd, "config", cfg.Get());
		proc_handler_call(entry.ph, "ptz_get_config", &cd);
		calldata_free(&cd);
		obs_data_array_push_back(configs, cfg);
	}
}

void PTZListModel::save(const QModelIndex &index, OBSData settings) const
{
	auto entry = entryAt(index);
	if (!entry)
		return;
	calldata_t cd = {};
	calldata_set_int(&cd, "device_id", entry->id);
	calldata_set_ptr(&cd, "config", settings.Get());
	proc_handler_call(entry->ph, "ptz_get_config", &cd);
	calldata_free(&cd);
}

void PTZListModel::preset_recall(uint32_t device_id, int preset_id)
{
	calldata_t cd = {};
	calldata_set_int(&cd, "device_id", device_id);
	calldata_set_int(&cd, "preset_id", preset_id);
	callDevice("ptz_preset_recall", &cd);
	calldata_free(&cd);
}

void PTZListModel::preset_save(uint32_t device_id, int preset_id)
{
	calldata_t cd = {};
	calldata_set_int(&cd, "device_id", device_id);
	calldata_set_int(&cd, "preset_id", preset_id);
	callDevice("ptz_preset_save", &cd);
	calldata_free(&cd);
}

void PTZListModel::deviceCreated(uint32_t device_id, proc_handler_t *ph, signal_handler_t *sh)
{
	PTZDeviceEntry entry;
	entry.id = device_id;
	entry.ph = ph;
	entry.sh = sh;
	devices.append(entry);
	rebuildRowIndex();

	/* Seed the cache; everything past this point is kept in sync purely
	 * by signal_handler notifications, never a direct method call. */
	refreshDeviceState(&devices.last());
	refreshPresetList(&devices.last());

	do_reset();

	signal_handler_connect(sh, "state_changed", device_state_changed_cb, this);
	signal_handler_connect(sh, "preset_insert", preset_insert_cb, this);
	signal_handler_connect(sh, "preset_inserted", preset_inserted_cb, this);
	signal_handler_connect(sh, "preset_remove", preset_remove_cb, this);
	signal_handler_connect(sh, "preset_removed", preset_removed_cb, this);
	signal_handler_connect(sh, "preset_move", preset_move_cb, this);
	signal_handler_connect(sh, "preset_moved", preset_moved_cb, this);
	signal_handler_connect(sh, "preset_renamed", preset_renamed_cb, this);
}

void PTZListModel::deviceDestroyed(uint32_t device_id)
{
	int row = rowByDeviceId.value(device_id, -1);
	if (row < 0)
		return;
	devices.removeAt(row);
	rebuildRowIndex();
	do_reset();
}

void PTZListModel::deviceStatusChanged(uint32_t device_id)
{
	auto entry = entryById(device_id);
	if (!entry)
		return;
	refreshDeviceState(entry);
	auto idx = indexFromDeviceId(device_id);
	if (idx.isValid())
		emit dataChanged(idx, idx);
}

void PTZListModel::presetsChanged(uint32_t device_id)
{
	auto entry = entryById(device_id);
	if (!entry)
		return;
	refreshPresetList(entry);
	if (entry->presets.isEmpty())
		return;
	auto parent = indexFromDeviceId(device_id);
	if (!parent.isValid())
		return;
	auto tl = index(0, 0, parent);
	auto br = index(entry->presets.size() - 1, 0, parent);
	if (tl.isValid() && br.isValid())
		emit dataChanged(tl, br);
}

void PTZListModel::presetBeginInsert(uint32_t device_id, int row)
{
	beginInsertRows(indexFromDeviceId(device_id), row, row);
}

void PTZListModel::presetEndInsert(uint32_t device_id)
{
	auto entry = entryById(device_id);
	if (entry)
		refreshPresetList(entry);
	endInsertRows();
}

void PTZListModel::presetBeginRemove(uint32_t device_id, int row)
{
	beginRemoveRows(indexFromDeviceId(device_id), row, row);
}

void PTZListModel::presetEndRemove(uint32_t device_id)
{
	auto entry = entryById(device_id);
	if (entry)
		refreshPresetList(entry);
	endRemoveRows();
}

bool PTZListModel::presetBeginMove(uint32_t device_id, int srcRow, int destRow)
{
	auto parent = indexFromDeviceId(device_id);
	return beginMoveRows(parent, srcRow, srcRow, parent, destRow);
}

void PTZListModel::presetEndMove(uint32_t device_id)
{
	auto entry = entryById(device_id);
	if (entry)
		refreshPresetList(entry);
	endMoveRows();
}
