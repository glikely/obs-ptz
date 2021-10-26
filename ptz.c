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
	blog(LOG_INFO, PLUGIN_FULL_NAME TOSTRING(PLUGIN_VERSION));
	ptz_load_devices();
	ptz_load_action_source();
	ptz_load_controls();
	ptz_load_settings();
	return true;
}

void obs_module_unload() {}

MODULE_EXPORT const char *obs_module_description(void)
{
	return obs_module_text("Pan, Tilt & Zoom control plugin");
}

MODULE_EXPORT const char *obs_module_name(void)
{
	return obs_module_text("PTZ Controls");
}
