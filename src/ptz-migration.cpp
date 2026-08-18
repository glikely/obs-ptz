/* Pan Tilt Zoom Controls - one-time legacy device migration
 *
 * Copyright 2020-2026 Grant Likely <grant.likely@secretlab.ca>
 *
 * SPDX-License-Identifier: GPLv2
 */

#include <obs-module.h>
#include <obs.hpp>
#include <obs-frontend-api.h>
#include <util/platform.h>
#include <cstring>
#include <string>
#include "ptz.h"

static bool devices_migrated = true;
static std::string legacy_devices_json;

/**
 * One-time migration of the legacy config.json "devices" array (from
 * before PTZDevice instances became filter-owned) into real "PTZ Control"
 * filters, run once a scene collection has actually finished loading (so
 * obs_get_source_by_name() below has something to find -- this can't run
 * from ptz_load_migration() itself, which happens at module-load time,
 * before any scene collection exists). Guarded by devices_migrated so it
 * only ever runs once per install: attaching a second filter for an
 * already-migrated device on every subsequent run would double up control
 * of the camera.
 */
static void migrate_legacy_devices()
{
	if (devices_migrated)
		return;
	/* Mark done, and persist that immediately, before touching anything
	 * below -- if migration only gets partway through (e.g. some sources
	 * are missing), retrying it next time would attach duplicate filters
	 * for the entries that already succeeded. Once is once, whether or
	 * not every entry found a home. */
	devices_migrated = true;
	char *file = obs_module_config_path("migrated.json");
	if (file) {
		OBSDataAutoRelease savedata = obs_data_create();
		obs_data_set_bool(savedata, "devices_migrated", true);
		if (!obs_data_save_json_pretty_safe(savedata, file, "tmp", "bak")) {
			char *path = obs_module_config_path("");
			if (path) {
				os_mkdirs(path);
				bfree(path);
			}
			obs_data_save_json_safe(savedata, file, "tmp", "bak");
		}
		bfree(file);
	}

	if (legacy_devices_json.empty())
		return;
	OBSDataAutoRelease wrapper = obs_data_create_from_json(legacy_devices_json.c_str());
	legacy_devices_json.clear();
	if (!wrapper)
		return;

	OBSDataArrayAutoRelease legacy_devices = obs_data_get_array(wrapper, "devices");
	for (size_t i = 0; i < obs_data_array_count(legacy_devices); i++) {
		OBSDataAutoRelease entry = obs_data_array_item(legacy_devices, i);
		const char *name = obs_data_get_string(entry, "name");
		OBSSourceAutoRelease parent = obs_get_source_by_name(name);
		if (!parent) {
			blog(LOG_WARNING,
			     "Migration: no source named '%s' found for a saved PTZ device -- add its filter manually",
			     name);
			continue;
		}
		OBSSourceAutoRelease filter =
			obs_source_create("PTZ Control", obs_module_text("PTZ.Filter.Name"), entry, nullptr);
		if (filter)
			obs_source_filter_add(parent, filter);
	}
}

static void ptz_migration_frontend_event(enum obs_frontend_event event, void *)
{
	if (event == OBS_FRONTEND_EVENT_FINISHED_LOADING)
		migrate_legacy_devices();
}

void ptz_load_migration(void)
{
	char *migrated_file = obs_module_config_path("migrated.json");
	if (migrated_file) {
		OBSDataAutoRelease migrated_data = obs_data_create_from_json_file_safe(migrated_file, "bak");
		bfree(migrated_file);
		if (migrated_data)
			devices_migrated = obs_data_get_bool(migrated_data, "devices_migrated");
		else
			devices_migrated = false;
	}

	if (!devices_migrated) {
		/* Stashed now, in memory, rather than re-read from config.json at
		 * migrate_legacy_devices() time: PTZControls::SaveConfig() rewrites
		 * that whole file and doesn't know about "devices" at all, so a
		 * save that lands before FINISHED_LOADING fires (ptz_load_controls(),
		 * which constructs PTZControls and registers its save callback,
		 * hasn't even run yet at this point) would silently drop this array
		 * before migration ever got to read it. */
		char *file = obs_module_config_path("config.json");
		if (file) {
			OBSDataAutoRelease loaddata = obs_data_create_from_json_file_safe(file, "bak");
			if (!loaddata) {
				/* Try loading from the old configuration path */
				std::string f = file;
				size_t pos = f.find("obs-ptz");
				if (pos != std::string::npos)
					f.replace(pos, strlen("obs-ptz"), "ptz-controls");
				loaddata = obs_data_create_from_json_file_safe(f.c_str(), "bak");
			}
			bfree(file);
			if (loaddata) {
				OBSDataArrayAutoRelease legacy_devices = obs_data_get_array(loaddata, "devices");
				if (legacy_devices && obs_data_array_count(legacy_devices) > 0) {
					OBSDataAutoRelease wrapper = obs_data_create();
					obs_data_set_array(wrapper, "devices", legacy_devices);
					legacy_devices_json = obs_data_get_json(wrapper);
				}
			}
		}
	}

	obs_frontend_add_event_callback(ptz_migration_frontend_event, nullptr);
}

void ptz_unload_migration(void)
{
	obs_frontend_remove_event_callback(ptz_migration_frontend_event, nullptr);
}
