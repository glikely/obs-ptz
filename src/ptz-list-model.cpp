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
 * device settings change callback-- connected to PTZDevice "ptz_settings_changed"
 * signal
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
static void preset_inserted_cb(void *data, calldata_t *cd)
{
	auto ptzlm = static_cast<PTZListModel *>(data);
	QString uuid = signalUuid(cd);
	auto row = (int)calldata_int(cd, "row");
	QMetaObject::invokeMethod(ptzlm, [ptzlm, uuid, row] { ptzlm->presetInserted(uuid, row); });
}

static void preset_removed_cb(void *data, calldata_t *cd)
{
	auto ptzlm = static_cast<PTZListModel *>(data);
	QString uuid = signalUuid(cd);
	auto row = (int)calldata_int(cd, "row");
	QMetaObject::invokeMethod(ptzlm, [ptzlm, uuid, row] { ptzlm->presetRemoved(uuid, row); });
}

static void preset_moved_cb(void *data, calldata_t *cd)
{
	auto ptzlm = static_cast<PTZListModel *>(data);
	QString uuid = signalUuid(cd);
	auto src_row = (int)calldata_int(cd, "src_row");
	auto dest_row = (int)calldata_int(cd, "dest_row");
	QMetaObject::invokeMethod(ptzlm,
				  [ptzlm, uuid, src_row, dest_row] { ptzlm->presetMoved(uuid, src_row, dest_row); });
}

/* A renamed preset and a new thumbnail both just mean "refetch the list" */
static void preset_renamed_cb(void *data, calldata_t *cd)
{
	auto ptzlm = static_cast<PTZListModel *>(data);
	QString uuid = signalUuid(cd);
	QMetaObject::invokeMethod(ptzlm, [ptzlm, uuid] { ptzlm->presetsChanged(uuid); });
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
	entry->live = obs_data_get_bool(state, "live");
	entry->preview = obs_data_get_bool(state, "preview");
	entry->locked = obs_data_get_bool(state, "locked");
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

/**
 * Re-fetches a device's preset list, plus the "preset_max" setting that caps
 * it, via a single ptz_preset_get_list() proc_handler call. Must be called
 * *before* the matching endInsertRows()/endRemoveRows()/endMoveRows() --
 * QAbstractItemModel requires the data to already reflect the new row layout
 * by the time the end*() call returns.
 */
void PTZListModel::refreshPresetList(PTZDeviceEntry *entry)
{
	entry->presets.clear();
	calldata_t cd = {};
	callEntry(*entry, "ptz_preset_get_list", &cd);
	auto list = static_cast<obs_data_array_t *>(calldata_ptr(&cd, "return"));
	if (list) {
		for (size_t i = 0; i < obs_data_array_count(list); i++) {
			OBSDataAutoRelease item = obs_data_array_item(list, i);
			PresetEntry preset;
			preset.id = (int)obs_data_get_int(item, "id");
			preset.name = QT_UTF8(obs_data_get_string(item, "name"));
			preset.token = QT_UTF8(obs_data_get_string(item, "token"));
			preset.thumbnail = QT_UTF8(obs_data_get_string(item, "thumbnail"));
			entry->presets.append(preset);
		}
		obs_data_array_release(list);
	}
	entry->maxPresets = (int)calldata_int(&cd, "max_presets");
	calldata_free(&cd);
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

bool PTZListModel::insertRows(int row, int count, const QModelIndex &parent)
{
	auto entry = entryAt(parent);
	if (!entry)
		return false;
	if (row < 0 || count <= 0 || row > entry->presets.size() || entry->presets.size() + count > entry->maxPresets)
		return false;

	for (int i = 0; i < count; i++) {
		calldata_t cd = {};
		calldata_set_int(&cd, "row", row++);
		callEntry(*entry, "ptz_preset_new", &cd);
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
		calldata_set_int(&cd, "row", row);
		callEntry(*entry, "ptz_preset_remove", &cd);
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
	 * presetMoved() would then have to reject via beginMoveRows(). */
	if (destChild == srcRow || destChild == srcRow + 1)
		return false;

	calldata_t cd = {};
	calldata_set_int(&cd, "src_row", srcRow);
	calldata_set_int(&cd, "dest_row", destChild);
	callEntry(*entry, "ptz_preset_move", &cd);
	calldata_free(&cd);
	return true;
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
			calldata_set_int(&cd, "id", entry->presets.at(index.row()).id);
			calldata_set_string(&cd, "name", QT_TO_UTF8(value.toString()));
			callEntry(*entry, "ptz_preset_set_name", &cd);
			calldata_free(&cd);
			/* cache refresh + dataChanged happen synchronously inside
			 * that call, via the ptz_preset_renamed signal */
			return true;
		}
		return false;
	}

	auto entry = entryAt(index.row());
	if (!entry)
		return false;

	if (role == PTZListModel::IsLockedRole && entry->locked != value.toBool()) {
		calldata_t cd = {};
		calldata_set_bool(&cd, "locked", value.toBool());
		callEntry(*entry, "ptz_set_locked", &cd);
		calldata_free(&cd);
		/* cache refresh + dataChanged happen synchronously inside that
		 * call, via the ptz_state_changed signal */
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
		callEntry(entry, "ptz_scene_changed", &cd);
	calldata_free(&cd);
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
	if (!entry)
		return nullptr;
	calldata_t cd = {};
	callEntry(*entry, "ptz_get_parent_source", &cd);
	OBSSourceAutoRelease source = static_cast<obs_source_t *>(calldata_ptr(&cd, "return"));
	calldata_free(&cd);
	if (source)
		return source.Get();
	/* A device that doesn't say is on its filter's parent, or is a source itself */
	return defaultParentSource(entry->weakSource).Get();
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

void PTZListModel::preset_recall(const QString &uuid, int preset_id)
{
	auto entry = entryByUuid(uuid);
	if (!entry)
		return;
	calldata_t cd = {};
	calldata_set_int(&cd, "preset_id", preset_id);
	callEntry(*entry, "ptz_preset_recall", &cd);
	calldata_free(&cd);
}

void PTZListModel::preset_save(const QString &uuid, int preset_id)
{
	auto entry = entryByUuid(uuid);
	if (!entry)
		return;
	calldata_t cd = {};
	calldata_set_int(&cd, "preset_id", preset_id);
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
	signal_handler_connect(sh, "ptz_settings_changed", device_settings_changed_cb, this);
	signal_handler_connect(sh, "ptz_preset_inserted", preset_inserted_cb, this);
	signal_handler_connect(sh, "ptz_preset_removed", preset_removed_cb, this);
	signal_handler_connect(sh, "ptz_preset_moved", preset_moved_cb, this);
	signal_handler_connect(sh, "ptz_preset_renamed", preset_renamed_cb, this);
	signal_handler_connect(sh, "ptz_preset_thumbnail_changed", preset_renamed_cb, this);
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

void PTZListModel::presetsChanged(const QString &uuid)
{
	auto entry = entryByUuid(uuid);
	if (!entry)
		return;
	refreshPresetList(entry);
	if (entry->presets.isEmpty())
		return;
	auto parent = indexFromUuid(uuid);
	if (!parent.isValid())
		return;
	auto tl = index(0, 0, parent);
	auto br = index(entry->presets.size() - 1, 0, parent);
	if (tl.isValid() && br.isValid())
		emit dataChanged(tl, br);
}

/**
 * PTZDevice has already inserted the new preset into its own list by the
 * time this fires; PTZListModel's cache (entry->presets) hasn't been
 * touched yet, so it's still reflecting the pre-insert row count -- exactly
 * what beginInsertRows() needs to see. Opening the begin/end bracket here,
 * around the cache refresh, is what QAbstractItemModel actually requires;
 * PTZDevice firing two calls instead of one wouldn't let it see anything
 * different.
 */
void PTZListModel::presetInserted(const QString &uuid, int row)
{
	auto entry = entryByUuid(uuid);
	if (!entry)
		return;
	beginInsertRows(indexFromUuid(uuid), row, row);
	refreshPresetList(entry);
	endInsertRows();
}

void PTZListModel::presetRemoved(const QString &uuid, int row)
{
	auto entry = entryByUuid(uuid);
	if (!entry)
		return;
	beginRemoveRows(indexFromUuid(uuid), row, row);
	refreshPresetList(entry);
	endRemoveRows();
}

void PTZListModel::presetMoved(const QString &uuid, int srcRow, int destRow)
{
	auto entry = entryByUuid(uuid);
	if (!entry)
		return;
	auto parent = indexFromUuid(uuid);
	if (!beginMoveRows(parent, srcRow, srcRow, parent, destRow))
		return;
	refreshPresetList(entry);
	endMoveRows();
}
