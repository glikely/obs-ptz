/* Pan Tilt Zoom OBS Plugin module
 *
 * Copyright 2020 Grant Likely <grant.likely@secretlab.ca>
 *
 * SPDX-License-Identifier: GPLv2
 *
 * Module initialization and utility routines for OBS
 */

#include <obs-module.h>
#include <util/config-file.h>
#include <util/platform.h>
#include "ptz.h"

OBS_DECLARE_MODULE();
OBS_MODULE_AUTHOR("Grant Likely <grant.likely@secretlab.ca>");
OBS_MODULE_USE_DEFAULT_LOCALE(PLUGIN_NAME, "en-US");

bool obs_module_load()
{
	blog(LOG_INFO, "plugin loaded successfully (version %s)",
	     PLUGIN_VERSION);
	ptz_load_devices();
	ptz_load_action_source();
	ptz_load_controls();
	ptz_load_settings();
	return true;
}

void obs_module_unload()
{
	ptz_unload_devices();
	blog(LOG_INFO, "plugin unloaded");
}

MODULE_EXPORT const char *obs_module_description(void)
{
	return obs_module_text("Pan, Tilt & Zoom control plugin");
}

MODULE_EXPORT const char *obs_module_name(void)
{
	return obs_module_text("PTZ Controls");
}

struct source_active_cb_data {
	obs_source_t *source;
	bool active;
};

static void source_active_cb(obs_source_t *parent, obs_source_t *child, void *data)
{
	UNUSED_PARAMETER(parent);
	struct source_active_cb_data *cb_data = data;
	if (child == cb_data->source)
		cb_data->active = true;
}

bool ptz_scene_is_source_active(obs_source_t *scene, obs_source_t *source)
{
	struct source_active_cb_data cb_data = { .source = source, .active = false };
	if (scene == source)
		return true;
	obs_source_enum_active_sources(scene, source_active_cb, &cb_data);
	return cb_data.active;
}
