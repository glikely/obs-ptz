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

/* The Windows and macOS builds are built against one OBS release (buildspec.json)
 * and run in whatever OBS the user has. OBS 32.0 and later refuse a plugin built
 * against a newer libobs than their own, major and minor version, but older ones
 * load it, and it then fails in ways that are hard to tell apart from a bug, such
 * as OBS 31 never quitting. Refuse it here as OBS 32 does. A Linux package is
 * built against its distro's libobs, and its package dependency says which. */
static bool ptz_obs_version_supported(void)
{
#if defined(_WIN32) || defined(__APPLE__)
	uint32_t built = LIBOBS_API_VER & 0xFFFF0000;
	uint32_t running = obs_get_version() & 0xFFFF0000;
	if (running < built) {
		blog(LOG_ERROR, "plugin not loaded: it needs OBS %u.%u or later, and this is OBS %s", built >> 24,
		     (built >> 16) & 0xFF, obs_get_version_string());
		return false;
	}
#endif
	return true;
}

bool obs_module_load()
{
	if (!ptz_obs_version_supported())
		return false;
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
