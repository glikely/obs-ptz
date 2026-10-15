/* Pan Tilt Zoom Controls - rolling backup of devices that have gone away
 *
 * Copyright 2026 Grant Likely <grant.likely@secretlab.ca>
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include "ptz-backup-store.hpp"

#include <ctime>
#include <obs.hpp>
#include <obs-module.h>
#include <util/platform.h>
#include <QHash>
#include <QMutex>
#include <QString>
#include <qt-wrappers.hpp>
#include "ptz.h"

/*
 * Each entry is the settings of a device's filter, which the device keeps
 * as what it saves (see persist() in ptz-device.cpp), with "name", "kind"
 * and "backup_time"; most recent first, one per source name and type. Kept
 * in its own file, loaded on first use, and guarded by backup_mutex since
 * OBS raises these signals from any thread.
 *
 * OBS says it three ways, all through its global signal handler. A filter
 * taken off its source ("source_filter_remove"), a source removed ("source_remove",
 * which is what deleting it, and switching scene collection, do to every
 * source: its filters are then backed up with it) and a filter destroyed
 * ("source_destroy"), where one that has no source by then is known by the
 * source it was on.
 */
#define PTZ_BACKUP_FILE "device-backups.json"
#define PTZ_BACKUP_MAX 32

static QMutex backup_mutex;
static OBSDataArray backups;
/* The name of the source each PTZ filter (by UUID) was last on, since a
 * filter that is destroyed has often been taken off it already */
static QHash<QString, QString> filter_sources;

/* Caller must hold backup_mutex */
static obs_data_array_t *backups_locked()
{
	if (backups)
		return backups;
	OBSDataAutoRelease data;
	char *file = obs_module_config_path(PTZ_BACKUP_FILE);
	if (file) {
		data = obs_data_create_from_json_file_safe(file, "bak");
		bfree(file);
	}
	OBSDataArrayAutoRelease array = data ? obs_data_get_array(data, "devices") : nullptr;
	if (!array)
		array = obs_data_array_create();
	backups = array.Get();
	return backups;
}

static void backups_write_locked()
{
	char *file = obs_module_config_path(PTZ_BACKUP_FILE);
	if (!file)
		return;
	OBSDataAutoRelease data = obs_data_create();
	obs_data_set_array(data, "devices", backups);
	/* Make the directory first, as a failed write is logged as an error */
	char *path = obs_module_config_path("");
	if (path) {
		os_mkdirs(path);
		bfree(path);
	}
	obs_data_save_json_pretty_safe(data, file, "tmp", "bak");
	bfree(file);
}

/* `filter` is a PTZ device, which was on the source called `source` */
static void backup_filter(obs_source_t *filter, const QString &source)
{
	/* Without a source there's nothing to recognise it by */
	if (source.isEmpty())
		return;
	/* What the device saved, as the filter holds it: its settings, with the
	 * defaults under them, which are a complete set as save() wrote it */
	OBSDataAutoRelease settings = obs_source_get_settings(filter);
	if (!settings)
		return;
	OBSDataAutoRelease entry = obs_data_get_defaults(settings);
	obs_data_apply(entry, settings);
	/* Not settings: the kind's marker (a default), and the identity that
	 * older versions kept in the settings */
	obs_data_erase(entry, PTZ_DEFAULTS_MAJOR_KEY);
	obs_data_erase(entry, PTZ_DEFAULTS_MINOR_KEY);
	obs_data_erase(entry, "id");
	obs_data_erase(entry, "is-self-managed");
	obs_data_set_string(entry, "name", QT_TO_UTF8(source));
	obs_data_set_string(entry, "kind", obs_source_get_id(filter));
	obs_data_set_int(entry, "backup_time", (long long)time(nullptr));
	QString type = QT_UTF8(obs_data_get_string(entry, "type"));

	QMutexLocker locker(&backup_mutex);
	obs_data_array_t *array = backups_locked();
	for (size_t i = obs_data_array_count(array); i-- > 0;) {
		OBSDataAutoRelease item = obs_data_array_item(array, i);
		if (source == QT_UTF8(obs_data_get_string(item, "name")) &&
		    type == QT_UTF8(obs_data_get_string(item, "type")))
			obs_data_array_erase(array, i);
	}
	obs_data_array_insert(array, 0, entry);
	while (obs_data_array_count(array) > PTZ_BACKUP_MAX)
		obs_data_array_erase(array, obs_data_array_count(array) - 1);
	backups_write_locked();
}

/* A PTZ filter, and the source it is, or was, on: back it up, and keep the
 * name for when it has none */
static void backup_on(obs_source_t *filter, obs_source_t *parent)
{
	if (!filter || !parent || !ptz_source_is_device(filter))
		return;
	QString name = QT_UTF8(obs_source_get_name(parent));
	{
		QMutexLocker locker(&backup_mutex);
		filter_sources.insert(QT_UTF8(obs_source_get_uuid(filter)), name);
	}
	backup_filter(filter, name);
}

/* "source_filter_add": its name, for when it is destroyed without one */
static void filter_added_cb(void *, calldata_t *cd)
{
	auto parent = static_cast<obs_source_t *>(calldata_ptr(cd, "source"));
	auto filter = static_cast<obs_source_t *>(calldata_ptr(cd, "filter"));
	if (!parent || !filter || !ptz_source_is_device(filter))
		return;
	QMutexLocker locker(&backup_mutex);
	filter_sources.insert(QT_UTF8(obs_source_get_uuid(filter)), QT_UTF8(obs_source_get_name(parent)));
}

/* "source_filter_remove": taken off its source by hand */
static void filter_removed_cb(void *, calldata_t *cd)
{
	backup_on(static_cast<obs_source_t *>(calldata_ptr(cd, "filter")),
		  static_cast<obs_source_t *>(calldata_ptr(cd, "source")));
}

/* "source_remove": the source is being deleted, and will take its filters
 * with it once the last reference to it goes, which can be much later */
static void source_removed_cb(void *, calldata_t *cd)
{
	auto source = static_cast<obs_source_t *>(calldata_ptr(cd, "source"));
	if (!source)
		return;
	obs_source_enum_filters(
		source, [](obs_source_t *parent, obs_source_t *filter, void *) { backup_on(filter, parent); }, nullptr);
}

/* "source_destroy": the filter, with the device it owns, is going */
static void source_destroyed_cb(void *, calldata_t *cd)
{
	auto source = static_cast<obs_source_t *>(calldata_ptr(cd, "source"));
	if (!source || obs_source_get_type(source) != OBS_SOURCE_TYPE_FILTER)
		return;
	if (!ptz_source_is_device(source))
		return;
	QString name;
	{
		QMutexLocker locker(&backup_mutex);
		name = filter_sources.take(QT_UTF8(obs_source_get_uuid(source)));
	}
	/* The source it is on, if it still is on one, is what it is called now */
	obs_source_t *parent = obs_filter_get_parent(source);
	if (parent)
		name = QT_UTF8(obs_source_get_name(parent));
	backup_filter(source, name);
}

void ptz_backup_store_start()
{
	signal_handler_t *global = obs_get_signal_handler();
	signal_handler_connect(global, "source_filter_add", filter_added_cb, nullptr);
	signal_handler_connect(global, "source_filter_remove", filter_removed_cb, nullptr);
	signal_handler_connect(global, "source_remove", source_removed_cb, nullptr);
	signal_handler_connect(global, "source_destroy", source_destroyed_cb, nullptr);
}

void ptz_backup_store_stop()
{
	signal_handler_t *global = obs_get_signal_handler();
	signal_handler_disconnect(global, "source_filter_add", filter_added_cb, nullptr);
	signal_handler_disconnect(global, "source_filter_remove", filter_removed_cb, nullptr);
	signal_handler_disconnect(global, "source_remove", source_removed_cb, nullptr);
	signal_handler_disconnect(global, "source_destroy", source_destroyed_cb, nullptr);

	QMutexLocker locker(&backup_mutex);
	backups = nullptr;
	filter_sources.clear();
}

obs_data_array_t *ptz_device_backups_get()
{
	QMutexLocker locker(&backup_mutex);
	obs_data_array_t *array = backups_locked();
	obs_data_array_t *copy = obs_data_array_create();
	for (size_t i = 0; i < obs_data_array_count(array); i++) {
		OBSDataAutoRelease item = obs_data_array_item(array, i);
		OBSDataAutoRelease entry = obs_data_create();
		obs_data_apply(entry, item);
		obs_data_array_push_back(copy, entry);
	}
	return copy;
}
