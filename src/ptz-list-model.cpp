/* Pan Tilt Zoom Controls - Qt list model for managing devices
 *
 * Copyright 2020-2026 Grant Likely <grant.likely@secretlab.ca>
 *
 * SPDX-License-Identifier: GPLv2
 */

#include <obs.hpp>
#include <qt-wrappers.hpp>
#include <QPixmap>
#include <QPixmapCache>
#include <obs-frontend-api.h>
#include "ptz-list-model.hpp"
#include "ptz.h"
#include "protocol-helpers.hpp"

PTZListModel *ptzDeviceList = nullptr;

/**
 * The trampolines below are signal_handler callbacks, which run
 * synchronously on whatever thread fired it. The model needs these
 * calls to happen on PTZListModels's own thread, so each decodes the
 * calldata arguments and uses QMetaObject::invokeMethod() to make the
 * method call on the correct thread.
 */
/* Which device a per-device signal is from: the UUID of the source it carries,
 * lent for the duration of the call */
static QString signalUuid(calldata_t *cd)
{
	auto source = static_cast<obs_source_t *>(calldata_ptr(cd, "source"));
	return source ? QString::fromUtf8(obs_source_get_uuid(source)) : QString();
}

/* The source a device is on, for one that doesn't say: its filter's parent, or
 * for a device that is a source, itself. */
static OBSSource defaultParentSource(const OBSWeakSource &weakSource)
{
	OBSSourceAutoRelease source = obs_weak_source_get_source(weakSource);
	if (!source)
		return nullptr;
	if (obs_source_get_type(source) != OBS_SOURCE_TYPE_FILTER)
		return source.Get();
	/* Borrowed from the filter; the OBSSource takes a reference of its own */
	return OBSSource(obs_filter_get_parent(source));
}

/* A source is a device of the PTZ API if it implements it, and that is all it takes,
 * whether it is a source or a filter and whichever plugin it is from.
 *
 * It is lent for the duration of the call, and the source, and its handlers, may be
 * gone by the time the queued call runs, so make a weak reference first. */
static void consider_source(PTZListModel *ptzlm, obs_source_t *source)
{
	if (!source || !ptz_source_is_device(source))
		return;
	OBSWeakSource weakSource = OBSGetWeakRef(source);
	QMetaObject::invokeMethod(ptzlm, [ptzlm, weakSource] { ptzlm->deviceCreated(weakSource); });
}

/* OBS says so, through its own global signal, every time a source is created:
 * a device that is a source. A source made private is not announced, and so is
 * not found. */
static void source_create_cb(void *data, calldata_t *cd)
{
	consider_source(static_cast<PTZListModel *>(data), static_cast<obs_source_t *>(calldata_ptr(cd, "source")));
}

/* And every time a filter is added to a source: a device that is a filter. A
 * filter is always private, and so never announces itself as a source being
 * created. */
static void filter_add_cb(void *data, calldata_t *cd)
{
	consider_source(static_cast<PTZListModel *>(data), static_cast<obs_source_t *>(calldata_ptr(cd, "filter")));
}

/* The source, and with it the device, is being destroyed: a source's own
 * "destroy" signal, whose calldata names the source as "source" */
static void filter_destroy_cb(void *data, calldata_t *cd)
{
	auto ptzlm = static_cast<PTZListModel *>(data);
	auto filter = static_cast<obs_source_t *>(calldata_ptr(cd, "source"));
	if (!filter)
		return;
	QString uuid = QString::fromUtf8(obs_source_get_uuid(filter));
	QMetaObject::invokeMethod(ptzlm, [ptzlm, uuid] { ptzlm->deviceDestroyed(uuid); });
}

/**
 * device stage change callback-- connected to PTZDevice "ptz_state_changed" signal
 */
static void device_state_changed_cb(void *data, calldata_t *cd)
{
	auto ptzlm = static_cast<PTZListModel *>(data);
	QString uuid = signalUuid(cd);
	/* The device never touches "changed" again once it has signalled it, so
	 * a reference to it, taken here, still holds what changed when the
	 * queued call runs */
	OBSData changed = static_cast<obs_data_t *>(calldata_ptr(cd, "changed"));
	QMetaObject::invokeMethod(ptzlm, [ptzlm, uuid, changed] { ptzlm->deviceStateChanged(uuid, changed); });
}

/**
 * device settings change callback-- connected to the source's "update" signal
 */
static void device_settings_changed_cb(void *data, calldata_t *cd)
{
	auto ptzlm = static_cast<PTZListModel *>(data);
	QString uuid = signalUuid(cd);
	QMetaObject::invokeMethod(ptzlm, [ptzlm, uuid] { ptzlm->deviceSettingsChanged(uuid); });
}

/**
 * Preset list mutation notifications. PTZDevice fires each of these once,
 * after its preset list has already been mutated -- these trampolines
 * route them to PTZListModel, which does the begin.../end...Rows()
 * bracketing itself against its own (still-stale) cache.
 */
/* Any change to the list of presets, or to one of them: the model finds out which */
static void preset_sync_cb(void *data, calldata_t *cd)
{
	auto ptzlm = static_cast<PTZListModel *>(data);
	QString uuid = signalUuid(cd);
	QMetaObject::invokeMethod(ptzlm, [ptzlm, uuid] { ptzlm->presetsSync(uuid); });
}

/* A request to make a preset has been answered: the id of the preset, or "" if none was made */
static void preset_create_done_cb(void *data, calldata_t *cd)
{
	auto ptzlm = static_cast<PTZListModel *>(data);
	QString uuid = signalUuid(cd);
	const char *request = "";
	const char *id = "";
	calldata_get_string(cd, "request", &request);
	calldata_get_string(cd, "id", &id);
	QString requested = QString::fromUtf8(request ? request : "");
	QString made = QString::fromUtf8(id ? id : "");
	QMetaObject::invokeMethod(ptzlm, [ptzlm, uuid, requested, made] {
		emit ptzlm->presetCreateDone(uuid, requested, made);
	});
}

PTZListModel::PTZListModel() : QAbstractItemModel()
{
	/* Plugins are loaded before any source is, so there are no devices to look for yet */
	signal_handler_t *global = obs_get_signal_handler();
	signal_handler_connect(global, "source_create", source_create_cb, this);
	signal_handler_connect(global, "source_filter_add", filter_add_cb, this);
}

PTZListModel::~PTZListModel()
{
	signal_handler_t *global = obs_get_signal_handler();
	signal_handler_disconnect(global, "source_create", source_create_cb, this);
	signal_handler_disconnect(global, "source_filter_add", filter_add_cb, this);
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
	rowBySerial.clear();
	rowByUuid.clear();
	for (int i = 0; i < devices.size(); i++) {
		rowBySerial[devices[i].serial] = i;
		rowByUuid[devices[i].uuid] = i;
	}
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
 * with their parent device's serial in internalId(), device rows with 0
 * (never a valid serial, see deviceCreated()). */
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

PTZListModel::PTZDeviceEntry *PTZListModel::entryBySerial(uint32_t serial)
{
	int row = rowBySerial.value(serial, -1);
	return row >= 0 ? &devices[row] : nullptr;
}

const PTZListModel::PTZDeviceEntry *PTZListModel::entryBySerial(uint32_t serial) const
{
	int row = rowBySerial.value(serial, -1);
	return row >= 0 ? &devices[row] : nullptr;
}

PTZListModel::PTZDeviceEntry *PTZListModel::entryByUuid(const QString &uuid)
{
	int row = rowByUuid.value(uuid, -1);
	return row >= 0 ? &devices[row] : nullptr;
}

const PTZListModel::PTZDeviceEntry *PTZListModel::entryByUuid(const QString &uuid) const
{
	int row = rowByUuid.value(uuid, -1);
	return row >= 0 ? &devices[row] : nullptr;
}

/**
 * Promotes entry.weakSource to a strong reference for the duration of the
 * call, guaranteeing the source's proc_handler is valid.
 */
bool PTZListModel::callEntry(const PTZDeviceEntry &entry, const char *method, calldata_t *cd) const
{
	OBSSourceAutoRelease source = obs_weak_source_get_source(entry.weakSource);
	if (!source)
		return false;
	proc_handler_t *ph = obs_source_get_proc_handler(source);
	return ph && proc_handler_call(ph, method, cd);
}

/**
 * Re-fetches everything data() needs to display a device row, via a single
 * ptz_get_state() proc_handler call. Called once to seed a new row, and
 * again whenever a ptz_state_changed signal says the cache may be stale.
 */
void PTZListModel::refreshDeviceState(PTZDeviceEntry *entry)
{
	OBSDataAutoRelease state = obs_data_create();

	calldata_t cd = {};
	calldata_set_ptr(&cd, "state", state.Get());
	callEntry(*entry, "ptz_get_state", &cd);
	calldata_free(&cd);

	if (obs_data_has_user_value(state, "source")) {
		entry->name = QT_UTF8(obs_data_get_string(state, "source"));
	} else {
		/* A device that doesn't say is called for its source */
		OBSSource parent = defaultParentSource(entry->weakSource);
		entry->name = parent ? QT_UTF8(obs_source_get_name(parent)) : QString();
	}
	entry->connected = obs_data_get_bool(state, "connected");
	/* A device can come to be on another source, or on its first, without
	 * the scene changing. Being locked follows being live, so a device that
	 * has just gone live is locked. */
	bool wasLive = entry->live;
	refreshSceneState(entry);
	if (entry->live != wasLive)
		entry->locked = entry->live;
	entry->poweredOff = obs_data_has_user_value(state, "power_on") && !obs_data_get_bool(state, "power_on");
	entry->features.reset();
	if (obs_data_has_user_value(state, "features")) {
		OBSDataAutoRelease features = obs_data_get_obj(state, "features");
		entry->features = QStringList();
		for (obs_data_item_t *item = obs_data_first(features); item; obs_data_item_next(&item)) {
			if (obs_data_item_get_bool(item))
				entry->features->append(QT_UTF8(obs_data_item_get_name(item)));
		}
	}
}

bool PTZListModel::refreshSceneState(PTZDeviceEntry *entry)
{
	bool wasLive = entry->live, wasPreview = entry->preview;
	entry->live = false;
	entry->preview = false;
	OBSSource source = parentSourceOf(*entry);
	if (source) {
		OBSSourceAutoRelease program = obs_frontend_get_current_scene();
		entry->live = ptz_scene_is_source_active(program, source);
		if (obs_frontend_preview_program_mode_active()) {
			OBSSourceAutoRelease previewScene = obs_frontend_get_current_preview_scene();
			entry->preview = ptz_scene_is_source_active(previewScene, source);
		}
	}
	return entry->live != wasLive || entry->preview != wasPreview;
}

/**
 * Re-fetches a device's preset list via a single ptz_preset_get_list()
 * proc_handler call. Must be called
 * *before* the matching endInsertRows()/endRemoveRows()/endMoveRows() --
 * QAbstractItemModel requires the data to already reflect the new row layout
 * by the time the end*() call returns.
 */
QList<PTZListModel::PresetEntry> PTZListModel::fetchPresets(const PTZDeviceEntry &entry) const
{
	QList<PresetEntry> presets;
	calldata_t cd = {};
	callEntry(entry, "ptz_preset_get_list", &cd);
	auto info = static_cast<obs_data_t *>(calldata_ptr(&cd, "return"));
	if (info) {
		OBSDataAutoRelease list = obs_data_get_obj(info, "presets");
		OBSDataArrayAutoRelease order = obs_data_get_array(info, "order");
		for (size_t i = 0; i < obs_data_array_count(order); i++) {
			OBSDataAutoRelease entry = obs_data_array_item(order, i);
			OBSDataAutoRelease item = obs_data_get_obj(list, obs_data_get_string(entry, "id"));
			if (!item)
				continue;
			PresetEntry preset;
			preset.id = QT_UTF8(obs_data_get_string(item, "id"));
			preset.store = QT_UTF8(obs_data_get_string(item, "store"));
			preset.name = QT_UTF8(obs_data_get_string(item, "name"));
			preset.thumbnail = QT_UTF8(obs_data_get_string(item, "thumbnail"));
			presets.append(preset);
		}
		obs_data_release(info);
	}
	calldata_free(&cd);
	return presets;
}

void PTZListModel::refreshPresetList(PTZDeviceEntry *entry)
{
	entry->presets = fetchPresets(*entry);
}

bool PTZListModel::hasFeature(const QModelIndex &index, const char *feature)
{
	if (!index.isValid())
		return false;
	QVariant features = index.data(FeaturesRole);
	return !features.isValid() || features.toStringList().contains(QString::fromUtf8(feature));
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
	return createIndex(row, column, (quintptr)entry->serial);
}

QModelIndex PTZListModel::parent(const QModelIndex &child) const
{
	quintptr serial = child.internalId();
	if (serial != 0) {
		int row = rowBySerial.value((uint32_t)serial, -1);
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

QString PTZListModel::addPreset(const QString &uuid, const QString &store, const QString &name)
{
	auto entry = entryByUuid(uuid);
	if (!entry)
		return QString();
	calldata_t cd = {};
	calldata_set_string(&cd, "name", QT_TO_UTF8(name));
	calldata_set_string(&cd, "store", QT_TO_UTF8(store));
	callEntry(*entry, "ptz_preset_create", &cd);
	const char *id = nullptr;
	calldata_get_string(&cd, "return", &id);
	QString created = QString::fromUtf8(id ? id : "");
	calldata_free(&cd);
	return created;
}

QStringList PTZListModel::presetStores(const QModelIndex &device) const
{
	QStringList stores;
	auto entry = entryAt(device);
	if (!entry)
		return stores;
	calldata_t cd = {};
	callEntry(*entry, "ptz_preset_get_list", &cd);
	auto info = static_cast<obs_data_t *>(calldata_ptr(&cd, "return"));
	if (info) {
		OBSDataAutoRelease available = obs_data_get_obj(info, "stores");
		for (const char *store : {"camera", "local"})
			if (obs_data_get_bool(available, store))
				stores << QString::fromUtf8(store);
		obs_data_release(info);
	}
	calldata_free(&cd);
	return stores;
}

bool PTZListModel::removeRows(int row, int count, const QModelIndex &parent)
{
	auto entry = entryAt(parent);
	if (!entry)
		return false;
	if (row < 0 || count <= 0 || row + count - 1 >= entry->presets.size())
		return false;

	/* The rows go as they are taken out, so each is the first of those left */
	QStringList ids;
	for (int i = 0; i < count; i++)
		ids << entry->presets.at(row + i).id;
	for (const QString &id : ids) {
		calldata_t cd = {};
		calldata_set_string(&cd, "id", QT_TO_UTF8(id));
		callEntry(*entry, "ptz_preset_delete", &cd);
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
	/* Same validity rule beginMoveRows() enforces (moving to a position
	 * within, or immediately after, the moved range is a no-op) -- check
	 * it here so the backend is never asked to perform a move
	 * presetsSync() would then have to reject via beginMoveRows(). */
	if (destChild == srcRow || destChild == srcRow + 1)
		return false;

	/* The device counts from the list as it is once the preset is out of it */
	calldata_t cd = {};
	calldata_set_string(&cd, "id", QT_TO_UTF8(entry->presets.at(srcRow).id));
	calldata_set_int(&cd, "index", destChild > srcRow ? destChild - 1 : destChild);
	callEntry(*entry, "ptz_preset_move", &cd);
	calldata_free(&cd);
	return true;
}

/* What to call a preset that has no name: its slot, if it is in one, or where it is in the list */
static int presetNumber(const PTZListModel::PresetEntry &preset, int row)
{
	bool isNumber = false;
	if (preset.store != QStringLiteral("camera"))
		return row + 1;
	int number = preset.id.section(QLatin1Char(':'), 1).toInt(&isNumber);
	return isNumber ? number : row + 1;
}

QVariant PTZListModel::data(const QModelIndex &index, int role) const
{
	if (!index.isValid())
		return QVariant();

	if (index.internalId() != 0) {
		/* Preset row -- internalId() is the parent device's serial */
		auto entry = entryBySerial((uint32_t)index.internalId());
		if (!entry || index.row() < 0 || index.row() >= entry->presets.size())
			return QVariant();
		const auto &preset = entry->presets.at(index.row());

		if (role == Qt::DisplayRole) {
			if (!preset.name.isEmpty())
				return preset.name;
			return QString(obs_module_text("PTZ.PresetNum")).arg(presetNumber(preset, index.row()));
		}
		if (role == Qt::ToolTipRole) {
			if (preset.store == QStringLiteral("local"))
				return QString(obs_module_text("PTZ.Preset.Tooltip.Local"));
			return QString(obs_module_text("PTZ.Preset.Tooltip")).arg(presetNumber(preset, index.row()));
		}
		if (role == Qt::EditRole)
			return preset.name;
		if (role == Qt::UserRole)
			return preset.id;
		if (role == PTZListModel::PresetStoreRole)
			return preset.store;
		if (role == PTZListModel::ThumbnailRole) {
			if (preset.thumbnail.isEmpty())
				return QPixmap();
			/* Kept in Qt's shared cache, so a refresh of the list
			 * doesn't reread every file. Each image has its own name. */
			QPixmap pixmap;
			if (!QPixmapCache::find(preset.thumbnail, &pixmap)) {
				pixmap.load(preset.thumbnail);
				QPixmapCache::insert(preset.thumbnail, pixmap);
			}
			return pixmap;
		}
		if (role == Qt::SizeHintRole)
			return QSize(0, 20);

		return QVariant();
	}

	auto entry = entryAt(index.row());
	if (!entry)
		return QVariant();

	if (role == Qt::DisplayRole || role == Qt::EditRole)
		return entry->name.isEmpty()
			       ? QString("%1 %2").arg(obs_module_text("PTZ.Device.DefaultName")).arg(entry->serial)
			       : entry->name;

	if (role == PTZListModel::DeviceUuidRole)
		return entry->uuid;

	if (role == PTZListModel::IsLiveRole)
		return entry->live;

	if (role == PTZListModel::IsPreviewRole)
		return entry->preview;

	if (role == PTZListModel::IsConnectedRole)
		return entry->connected;

	if (role == PTZListModel::IsPoweredOffRole)
		return entry->poweredOff;

	if (role == PTZListModel::IsLockedRole)
		return entry->locked;

	if (role == PTZListModel::FeaturesRole)
		return entry->features ? QVariant(*entry->features) : QVariant();

	return QVariant();
}

bool PTZListModel::setData(const QModelIndex &index, const QVariant &value, int role)
{
	if (index.internalId() != 0) {
		auto entry = entryBySerial((uint32_t)index.internalId());
		if (!entry || index.row() < 0 || index.row() >= entry->presets.size())
			return false;

		if (role == Qt::EditRole) {
			calldata_t cd = {};
			OBSDataAutoRelease changes = obs_data_create();
			obs_data_set_string(changes, "name", QT_TO_UTF8(value.toString()));
			calldata_set_string(&cd, "id", QT_TO_UTF8(entry->presets.at(index.row()).id));
			calldata_set_ptr(&cd, "changes", changes.Get());
			callEntry(*entry, "ptz_preset_update", &cd);
			calldata_free(&cd);
			/* cache refresh + dataChanged happen synchronously inside
			 * that call, via the ptz_preset_changed signal */
			return true;
		}
		return false;
	}

	auto entry = entryAt(index.row());
	if (!entry)
		return false;

	if (role == PTZListModel::IsLockedRole && entry->locked != value.toBool()) {
		entry->locked = value.toBool();
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

void PTZListModel::onSceneChanged()
{
	for (int row = 0; row < devices.size(); row++) {
		auto &entry = devices[row];
		bool wasLocked = entry.locked;
		bool changed = refreshSceneState(&entry);
		entry.locked = entry.live;
		if (changed || entry.locked != wasLocked)
			emit dataChanged(index(row, 0), index(row, 0));
	}
}

/**
 * This variant of callDevice accepts a QModelIndex into the data model
 * to choose which device will receive the call
 */
bool PTZListModel::callDevice(const QModelIndex &index, const char *method, calldata_t *cd)
{
	auto entry = entryAt(index);
	return entry ? callEntry(*entry, method, cd) : false;
}

QModelIndex PTZListModel::indexFromUuid(const QString &uuid) const
{
	int row = rowByUuid.value(uuid, -1);
	return row >= 0 ? index(row, 0) : QModelIndex();
}

QModelIndex PTZListModel::indexFromSource(obs_source_t *source) const
{
	if (!source)
		return QModelIndex();
	for (int row = 0; row < devices.size(); row++)
		if (obs_weak_source_references_source(devices.at(row).weakSource, source))
			return index(row, 0);
	return QModelIndex();
}

OBSSource PTZListModel::parentSource(const QModelIndex &index) const
{
	auto entry = entryAt(index);
	return entry ? parentSourceOf(*entry) : nullptr;
}

OBSSource PTZListModel::parentSourceOf(const PTZDeviceEntry &entry) const
{
	calldata_t cd = {};
	callEntry(entry, "ptz_get_parent_source", &cd);
	OBSSourceAutoRelease source = static_cast<obs_source_t *>(calldata_ptr(&cd, "return"));
	calldata_free(&cd);
	if (source)
		return source.Get();
	/* A device that doesn't say is on its filter's parent, or is a source itself */
	return defaultParentSource(entry.weakSource).Get();
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

/* The device's settings are its source's settings, with what each is by default */
void PTZListModel::save(const QModelIndex &index, OBSData settings) const
{
	auto entry = entryAt(index);
	if (!entry)
		return;
	OBSSourceAutoRelease source = obs_weak_source_get_source(entry->weakSource);
	if (!source)
		return;
	OBSDataAutoRelease live = obs_source_get_settings(source);
	OBSDataAutoRelease complete = obs_data_get_defaults(live);
	obs_data_apply(complete, live);
	obs_data_apply(settings, complete);
}

/* Changes the settings of the device's source. The device applies them when the
 * source updates, which is not before this returns: the model refreshes when
 * the device says its settings changed. */
void PTZListModel::update(const QModelIndex &index, OBSData settings)
{
	auto entry = entryAt(index);
	if (!entry)
		return;
	OBSSourceAutoRelease source = obs_weak_source_get_source(entry->weakSource);
	if (!source)
		return;
	OBSDataAutoRelease changes = obs_data_create();
	obs_data_apply(changes, settings);
	obs_source_update(source, changes);
}

/**
 * The state half of save()/update(): the device's whole transient state,
 * and a request to change some of it. See PTZDevice::saveState()/
 * requestState().
 */
void PTZListModel::saveState(const QModelIndex &index, OBSData state) const
{
	auto entry = entryAt(index);
	if (!entry)
		return;
	calldata_t cd = {};
	calldata_set_ptr(&cd, "state", state.Get());
	callEntry(*entry, "ptz_get_state", &cd);
	calldata_free(&cd);
	/* What the device list knows of the device, which the device does not */
	obs_data_set_bool(state, "live", entry->live);
	obs_data_set_bool(state, "preview", entry->preview);
	obs_data_set_bool(state, "locked", entry->locked);
}

void PTZListModel::setState(const QModelIndex &index, OBSData state)
{
	auto entry = entryAt(index);
	if (!entry)
		return;
	calldata_t cd = {};
	calldata_set_ptr(&cd, "state", state.Get());
	callEntry(*entry, "ptz_request_state", &cd);
	calldata_free(&cd);
}

obs_properties_t *PTZListModel::getProperties(const QModelIndex &index) const
{
	auto entry = entryAt(index);
	if (!entry)
		return obs_properties_create();
	/* The device's source says them, as OBS asks any source */
	OBSSourceAutoRelease source = obs_weak_source_get_source(entry->weakSource);
	obs_properties_t *props = source ? obs_source_properties(source) : nullptr;
	return props ? props : obs_properties_create();
}

/* A device that is a filter goes with its filter, so remove that from its
 * source; the device is backed up as it is destroyed. One that is a source is
 * the user's source, which is theirs to remove from OBS, not from here. */
void PTZListModel::removeDevice(const QModelIndex &index)
{
	auto entry = entryAt(index);
	if (!entry)
		return;
	OBSSourceAutoRelease source = obs_weak_source_get_source(entry->weakSource);
	if (!source || obs_source_get_type(source) != OBS_SOURCE_TYPE_FILTER)
		return;
	obs_source_t *parent = obs_filter_get_parent(source);
	if (parent)
		obs_source_filter_remove(parent, source);
}

void PTZListModel::preset_recall(const QString &uuid, const QString &preset_id)
{
	auto entry = entryByUuid(uuid);
	if (!entry)
		return;
	calldata_t cd = {};
	calldata_set_string(&cd, "id", QT_TO_UTF8(preset_id));
	callEntry(*entry, "ptz_preset_recall", &cd);
	calldata_free(&cd);
}

void PTZListModel::preset_save(const QString &uuid, const QString &preset_id)
{
	auto entry = entryByUuid(uuid);
	if (!entry)
		return;
	calldata_t cd = {};
	calldata_set_string(&cd, "id", QT_TO_UTF8(preset_id));
	callEntry(*entry, "ptz_preset_save", &cd);
	calldata_free(&cd);
}

void PTZListModel::deviceCreated(OBSWeakSource weakSource)
{
	/* Not there to be asked about if its source has gone since it was announced */
	OBSSourceAutoRelease source = obs_weak_source_get_source(weakSource);
	if (!source)
		return;
	/* A filter can be added to a source more than once */
	QString uuid = QString::fromUtf8(obs_source_get_uuid(source));
	if (rowByUuid.contains(uuid))
		return;
	signal_handler_t *sh = obs_source_get_signal_handler(source);

	PTZDeviceEntry entry;
	entry.serial = nextSerial++;
	/* Kept, since once the source is destroyed there is no asking it */
	entry.uuid = uuid;
	entry.weakSource = weakSource;
	devices.append(entry);
	rebuildRowIndex();

	/* Seed the cache; everything past this point is kept in sync purely
	 * by signal_handler notifications, never a direct method call. */
	refreshDeviceState(&devices.last());
	refreshPresetList(&devices.last());

	do_reset();

	signal_handler_connect(sh, "destroy", filter_destroy_cb, this);
	signal_handler_connect(sh, "ptz_state_changed", device_state_changed_cb, this);
	signal_handler_connect(sh, "update", device_settings_changed_cb, this);
	for (const char *name : {"ptz_preset_added", "ptz_preset_removed", "ptz_preset_order_changed",
				 "ptz_preset_changed", "ptz_preset_list_reset"})
		signal_handler_connect(sh, name, preset_sync_cb, this);
	signal_handler_connect(sh, "ptz_preset_create_done", preset_create_done_cb, this);
}

void PTZListModel::deviceDestroyed(const QString &uuid)
{
	int row = rowByUuid.value(uuid, -1);
	if (row < 0)
		return;
	devices.removeAt(row);
	rebuildRowIndex();
	do_reset();
}

void PTZListModel::deviceStateChanged(const QString &uuid, OBSData changed)
{
	auto entry = entryByUuid(uuid);
	if (!entry)
		return;
	refreshDeviceState(entry);
	auto idx = indexFromUuid(uuid);
	if (idx.isValid())
		emit dataChanged(idx, idx);
	emit deviceStateUpdated(uuid, changed);
}

/* The settings can change behind the model's back -- OBS's Filters dialog
 * and obs-websocket both edit a filter's settings without asking it -- so
 * this is where it finds out, whether or not update() below was the cause. */
void PTZListModel::deviceSettingsChanged(const QString &uuid)
{
	auto entry = entryByUuid(uuid);
	if (!entry)
		return;
	refreshDeviceState(entry);
	refreshPresetList(entry);
	auto idx = indexFromUuid(uuid);
	if (idx.isValid())
		emit dataChanged(idx, idx);
	emit deviceSettingsUpdated(uuid);
}

/**
 * The device's list of presets changed in some way: a preset was added or
 * removed or changed, or the order, or all of it. The signals don't say where,
 * so this finds out, and tells the model's views in the terms they want: rows
 * removed, rows inserted, rows moved, and rows changed. The cache is made current
 * between the begin and end of each, as QAbstractItemModel requires.
 */
void PTZListModel::presetsSync(const QString &uuid)
{
	auto entry = entryByUuid(uuid);
	if (!entry)
		return;
	auto parent = indexFromUuid(uuid);
	QList<PresetEntry> now = fetchPresets(*entry);

	QStringList wanted;
	for (const PresetEntry &preset : now)
		wanted << preset.id;

	/* What is gone, from the end */
	for (int row = entry->presets.size() - 1; row >= 0; row--) {
		if (wanted.contains(entry->presets.at(row).id))
			continue;
		beginRemoveRows(parent, row, row);
		entry->presets.removeAt(row);
		endRemoveRows();
	}
	/* What is new, where it goes */
	for (int row = 0; row < now.size(); row++) {
		bool known = false;
		for (const PresetEntry &have : entry->presets)
			known = known || have.id == now.at(row).id;
		if (known)
			continue;
		beginInsertRows(parent, row, row);
		entry->presets.insert(row, now.at(row));
		endInsertRows();
	}
	/* What is where it was not, moved up to where it goes */
	for (int row = 0; row < now.size(); row++) {
		if (entry->presets.at(row).id == now.at(row).id)
			continue;
		int from = row + 1;
		while (from < entry->presets.size() && entry->presets.at(from).id != now.at(row).id)
			from++;
		if (from >= entry->presets.size())
			break;
		if (!beginMoveRows(parent, from, from, parent, row))
			break;
		entry->presets.move(from, row);
		endMoveRows();
	}
	/* And what changed in them */
	entry->presets = now;
	if (!entry->presets.isEmpty() && parent.isValid()) {
		auto tl = index(0, 0, parent);
		auto br = index(entry->presets.size() - 1, 0, parent);
		if (tl.isValid() && br.isValid())
			emit dataChanged(tl, br);
	}
}
