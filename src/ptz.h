/* Pan Tilt Zoom OBS Plugin module
 *
 * Copyright 2020 Grant Likely <grant.likely@secretlab.ca>
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * Module initialization and utility routines for OBS
 */

#ifndef PTZ_H
#define PTZ_H

#include <obs-module.h>

#if LIBOBS_API_VER < MAKE_SEMANTIC_VERSION(31, 0, 0)
#error "obs-ptz needs OBS Studio 31 or later: libobs-dev is too old"
#endif

#define blog(level, msg, ...) blog(level, "[ptz] " msg, ##__VA_ARGS__)
#define STRINGIFY(x) #x
#define TOSTRING(x) STRINGIFY(x)

#ifdef __cplusplus
extern "C" {
#endif

extern const char *ptz_plugin_version;

extern void ptz_load_devices(void);
extern void ptz_unload_devices(void);
extern void ptz_load_action_source(void);
extern void ptz_load_controls(void);
extern void ptz_load_settings(void);
#ifdef ENABLE_UI_TESTS
extern void ptz_load_ui_tests(void);
#else
static inline void ptz_load_ui_tests(void) {};
#endif

/* Settings of devices that were recently destroyed, most recent first: each
 * what the device saved, with the "name" of its source and "backup_time" (in
 * seconds since the epoch). Returns a new reference. */
extern obs_data_array_t *ptz_device_backups_get(void);
/* The "PTZ Control" filter kind for a device "type", or NULL if none */
extern const char *ptz_device_filter_kind(const char *type);
/* Adds a PTZ Control filter, and so a device, to `parent`, for the "type"
 * in `config` and with its settings. Returns a new reference to the filter,
 * or NULL. */
extern obs_source_t *ptz_device_create_filter(obs_source_t *parent, obs_data_t *config);

/* Whether a source is a device of the PTZ API, from this plugin or from any
 * other, a filter or not: its proc_handler answers ptz_get_api_version with
 * the same major version as this plugin's and a minor version at least as new
 * (see docs/ptz-device-api.md). Nothing about its id says so. */
extern bool ptz_source_is_device(obs_source_t *source);

/* Does what a device does when OBS has finished loading, for a device made
 * while it was doing that, which is too late to have heard of it */
extern void ptz_device_startup(obs_source_t *filter);

extern bool ptz_scene_is_source_active(obs_source_t *scene, obs_source_t *source);

/* The version of the PTZ API that ptz_get_api_version reports, on each of this
 * plugin's devices (there is no version of the plugin as a whole): the procs and
 * signals in docs/ptz-device-api.md, and the keys and names they take. Bump
 * the minor version for a change an existing caller can't notice (something
 * added), and the major version, resetting the minor, for one it can
 * (something removed, renamed or changing meaning). Then update the doc,
 * which states it. */
#define PTZ_API_VERSION_MAJOR 0
#define PTZ_API_VERSION_MINOR 2

#ifdef __cplusplus
}
#endif

#endif /* PTZ_H */
