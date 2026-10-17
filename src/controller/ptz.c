/* Pan Tilt Zoom OBS Plugin module
 *
 * Copyright 2020 Grant Likely <grant.likely@secretlab.ca>
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * Module initialization and utility routines for OBS
 */

#include <obs-module.h>
#include <util/config-file.h>
#include <util/platform.h>
#include "ptz.h"

OBS_DECLARE_MODULE();
OBS_MODULE_AUTHOR("Grant Likely <grant.likely@secretlab.ca>");
OBS_MODULE_USE_DEFAULT_LOCALE("obs-ptz", "en-GB");

bool obs_module_load()
{
	blog(LOG_INFO, "plugin loaded successfully (version %s)", ptz_plugin_version);
	ptz_load_devices();
	ptz_load_action_source();
	ptz_load_controls();
	ptz_load_settings();
	return true;
}

void obs_module_post_load(void)
{
	ptz_websocket_load();
}

void obs_module_unload()
{
	ptz_websocket_unload();
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
