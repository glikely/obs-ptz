/* Pan Tilt Zoom Controls - migrate devices saved by the old self-managed backend
 *
 * Copyright 2026 Grant Likely <grant.likely@secretlab.ca>
 *
 * SPDX-License-Identifier: GPLv2
 */

#include <obs-module.h>
#include <obs-frontend-api.h>
#include <obs.hpp>
#include <QDir>
#include <QFile>
#include <QHash>
#include <QList>
#include <QSet>
#include <QString>
#include <QStringList>
#include <qt-wrappers.hpp>
#include "ptz-legacy-migration.hpp"
#include "ptz.h"

static OBSDataArray legacy_devices;
/* The UUID of the filter of each old device, by the id it had */
static QHash<uint32_t, QByteArray> legacy_id_map;

static const char *const CONFIG_BACKUP_SUFFIX = ".pre-filter-migration";
static const char *const COLLECTION_BACKUP_SUFFIX = ".pre-ptz-filter-migration";

/* Which collections an entry has been made into filters in. Old versions ignore
 * the key, so downgrading still finds the entry */
static const char *const MIGRATED_KEY = "migrated_collections";

static void backup_file(const QString &path, const char *suffix)
{
	QString dest = path + suffix;
	if (QFile::exists(dest))
		return;
	if (!QFile::copy(path, dest))
		blog(LOG_WARNING, "could not back up %s to %s", QT_TO_UTF8(path), QT_TO_UTF8(dest));
	else
		blog(LOG_INFO, "backed up %s to %s", QT_TO_UTF8(path), QT_TO_UTF8(dest));
}

void ptz_legacy_load(const char *file, obs_data_t *config)
{
	OBSDataArrayAutoRelease array = obs_data_get_array(config, "devices");
	if (!array || !obs_data_array_count(array))
		return;
	if (file)
		backup_file(QT_UTF8(file), CONFIG_BACKUP_SUFFIX);
	legacy_devices = array.Get();
	blog(LOG_INFO, "%zu device(s) to migrate to filters", obs_data_array_count(array));
}

obs_data_array_t *ptz_legacy_devices_save()
{
	obs_data_array_t *copy = obs_data_array_create();
	if (!legacy_devices)
		return copy;
	for (size_t i = 0; i < obs_data_array_count(legacy_devices); i++) {
		OBSDataAutoRelease item = obs_data_array_item(legacy_devices, i);
		OBSDataAutoRelease entry = obs_data_create();
		obs_data_apply(entry, item);
		obs_data_array_push_back(copy, entry);
	}
	return copy;
}

void ptz_legacy_unload()
{
	legacy_devices = nullptr;
	legacy_id_map.clear();
}

const char *ptz_legacy_remap_id(uint32_t old_id)
{
	auto it = legacy_id_map.constFind(old_id);
	return it == legacy_id_map.constEnd() ? nullptr : it->constData();
}

struct Collection {
	QString name;
	QString path;
	QSet<QString> sources;
};

/* OBS keeps each scene collection in a file of its own, in basic/scenes next
 * to plugin_config. Read just what is needed: their names, and the names of
 * their sources. */
static QList<Collection> scan_collections()
{
	QList<Collection> found;
	char *module_path = obs_module_config_path("");
	if (!module_path)
		return found;
	QDir dir(QDir(QT_UTF8(module_path)).absoluteFilePath("../../basic/scenes"));
	bfree(module_path);
	for (const QFileInfo &info : dir.entryInfoList({"*.json"}, QDir::Files)) {
		OBSDataAutoRelease data = obs_data_create_from_json_file(QT_TO_UTF8(info.absoluteFilePath()));
		if (!data)
			continue;
		Collection c;
		c.name = QT_UTF8(obs_data_get_string(data, "name"));
		c.path = info.absoluteFilePath();
		OBSDataArrayAutoRelease sources = obs_data_get_array(data, "sources");
		for (size_t i = 0; i < obs_data_array_count(sources); i++) {
			OBSDataAutoRelease source = obs_data_array_item(sources, i);
			c.sources.insert(QT_UTF8(obs_data_get_string(source, "name")));
		}
		found.append(c);
	}
	return found;
}

static QString current_collection()
{
	char *name = obs_frontend_get_current_scene_collection();
	QString s = QT_UTF8(name);
	bfree(name);
	return s;
}

/* A source for a device that has none to go on. It is held by a hidden item
 * in the current scene: a source nothing holds is not saved with the collection. */
static obs_source_t *make_placeholder(const QString &wanted, uint32_t old_id)
{
	OBSSourceAutoRelease scene_source = obs_frontend_get_current_scene();
	obs_scene_t *scene = scene_source ? obs_scene_from_source(scene_source) : nullptr;
	if (!scene)
		return nullptr;

	QString base = wanted;
	if (base.isEmpty())
		base = QString("%1 %2").arg(obs_module_text("PTZ.Device.DefaultName")).arg(old_id);
	QString name = base;
	for (int i = 2;; i++) {
		OBSSourceAutoRelease existing = obs_get_source_by_name(QT_TO_UTF8(name));
		if (!existing)
			break;
		name = QString("%1 %2").arg(base).arg(i);
	}

	OBSDataAutoRelease settings = obs_data_create();
	obs_data_set_int(settings, "width", 1920);
	obs_data_set_int(settings, "height", 1080);
	obs_data_set_int(settings, "color", 0xFF202020);
	obs_source_t *source = obs_source_create("color_source_v3", QT_TO_UTF8(name), settings, nullptr);
	if (!source)
		return nullptr;
	obs_sceneitem_t *item = obs_scene_add(scene, source);
	if (item)
		obs_sceneitem_set_visible(item, false);
	blog(LOG_INFO, "made placeholder source '%s' for device %u", QT_TO_UTF8(name), old_id);
	return source;
}

struct FindFilter {
	const char *kind;
	obs_source_t *found;
};

/* Action sources name a camera by the id of its old device; they now name its
 * filter, so move the ones whose camera has just got one */
static void remap_action_sources(const QHash<uint32_t, QByteArray> &map)
{
	if (map.isEmpty())
		return;
	auto cb = [](void *param, obs_source_t *source) {
		auto map = static_cast<const QHash<uint32_t, QByteArray> *>(param);
		if (strcmp(obs_source_get_id(source), "ptz_action_source") != 0)
			return true;
		OBSDataAutoRelease settings = obs_source_get_settings(source);
		if (obs_data_has_user_value(settings, "device_uuid") || !obs_data_has_user_value(settings, "device_id"))
			return true;
		uint32_t old_id = (uint32_t)obs_data_get_int(settings, "device_id");
		auto it = map->constFind(old_id);
		if (it == map->constEnd())
			return true;
		obs_data_erase(settings, "device_id");
		OBSDataAutoRelease update = obs_data_create();
		obs_data_set_string(update, "device_uuid", it->constData());
		obs_source_update(source, update);
		blog(LOG_INFO, "action source '%s' now uses the filter %s (was device %u)", obs_source_get_name(source),
		     it->constData(), old_id);
		return true;
	};
	obs_enum_sources(cb, const_cast<QHash<uint32_t, QByteArray> *>(&map));
}

bool ptz_legacy_migrate(bool finishing_loading)
{
	if (!legacy_devices || !obs_data_array_count(legacy_devices))
		return false;
	QString current = current_collection();
	if (current.isEmpty())
		return false;

	QList<Collection> collections = scan_collections();
	auto elsewhere = [&](const QString &source) {
		for (const auto &c : collections)
			if (c.name != current && c.sources.contains(source))
				return true;
		return false;
	};

	bool changed = false, backed_up = false;
	QHash<uint32_t, QByteArray> pass_map;

	for (size_t i = 0; i < obs_data_array_count(legacy_devices); i++) {
		OBSDataAutoRelease item = obs_data_array_item(legacy_devices, i);
		QString name = QT_UTF8(obs_data_get_string(item, "name"));
		uint32_t old_id = (uint32_t)obs_data_get_int(item, "id");
		const char *kind = ptz_device_filter_kind(obs_data_get_string(item, "type"));

		OBSDataArrayAutoRelease done_array = obs_data_get_array(item, MIGRATED_KEY);
		QStringList done;
		for (size_t j = 0; done_array && j < obs_data_array_count(done_array); j++) {
			OBSDataAutoRelease d = obs_data_array_item(done_array, j);
			done.append(QT_UTF8(obs_data_get_string(d, "name")));
		}
		if (done.contains(current))
			continue;
		if (!kind) {
			blog(LOG_WARNING, "device %u: no filter for type '%s', left in config.json", old_id,
			     obs_data_get_string(item, "type"));
			continue;
		}

		OBSSourceAutoRelease parent = name.isEmpty() ? nullptr : obs_get_source_by_name(QT_TO_UTF8(name));
		if (!parent) {
			/* Another collection has it: wait for that one to load */
			if (!name.isEmpty() && elsewhere(name))
				continue;
			/* Only one collection gets the placeholder of a device with no source */
			if (!done.isEmpty())
				continue;
			parent = make_placeholder(name, old_id);
			if (!parent)
				continue;
		}

		if (!backed_up) {
			for (const auto &c : collections)
				if (c.name == current)
					backup_file(c.path, COLLECTION_BACKUP_SUFFIX);
			backed_up = true;
		}

		/* A filter of the kind may be there already, from an earlier attempt */
		FindFilter ff = {kind, nullptr};
		obs_source_enum_filters(
			parent,
			[](obs_source_t *, obs_source_t *filter, void *p) {
				auto ff = static_cast<FindFilter *>(p);
				if (!ff->found && strcmp(obs_source_get_id(filter), ff->kind) == 0)
					ff->found = filter;
			},
			&ff);
		OBSSourceAutoRelease filter;
		if (ff.found) {
			filter = obs_source_get_ref(ff.found);
		} else {
			filter = ptz_device_create_filter(parent, item);
			/* OBS refuses it, without saying, on a source that can't take it */
			if (filter && obs_filter_get_parent(filter) != parent) {
				blog(LOG_WARNING, "device %u: '%s' can not take the filter, using a placeholder",
				     old_id, obs_source_get_name(parent));
				filter = nullptr;
				parent = make_placeholder(QString(obs_source_get_name(parent)) + " PTZ", old_id);
				if (parent)
					filter = ptz_device_create_filter(parent, item);
			}
		}
		if (!filter) {
			blog(LOG_WARNING, "device %u: could not make a filter on '%s'", old_id,
			     obs_source_get_name(parent));
			continue;
		}

		if (finishing_loading)
			ptz_device_startup(filter);
		QByteArray uuid = obs_source_get_uuid(filter);
		pass_map.insert(old_id, uuid);
		legacy_id_map.insert(old_id, uuid);
		blog(LOG_INFO, "migrated device %u to a filter on '%s' (%s)", old_id, obs_source_get_name(parent),
		     uuid.constData());

		if (!done_array)
			done_array = obs_data_array_create();
		OBSDataAutoRelease d = obs_data_create();
		obs_data_set_string(d, "name", QT_TO_UTF8(current));
		obs_data_array_push_back(done_array, d);
		obs_data_set_array(item, MIGRATED_KEY, done_array);
		changed = true;
	}

	/* An entry is finished once it is in a collection and no other one still
	 * has its source */
	for (size_t i = obs_data_array_count(legacy_devices); i-- > 0;) {
		OBSDataAutoRelease item = obs_data_array_item(legacy_devices, i);
		OBSDataArrayAutoRelease done_array = obs_data_get_array(item, MIGRATED_KEY);
		if (!done_array || !obs_data_array_count(done_array))
			continue;
		QString name = QT_UTF8(obs_data_get_string(item, "name"));
		QStringList done;
		for (size_t j = 0; j < obs_data_array_count(done_array); j++) {
			OBSDataAutoRelease d = obs_data_array_item(done_array, j);
			done.append(QT_UTF8(obs_data_get_string(d, "name")));
		}
		bool pending = false;
		for (const auto &c : collections)
			if (!name.isEmpty() && c.sources.contains(name) && !done.contains(c.name))
				pending = true;
		if (!pending) {
			obs_data_array_erase(legacy_devices, i);
			changed = true;
		}
	}

	remap_action_sources(pass_map);
	if (!pass_map.isEmpty())
		obs_frontend_save();
	return changed;
}
