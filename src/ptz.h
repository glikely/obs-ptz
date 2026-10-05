/* Pan Tilt Zoom OBS Plugin module
 *
 * Copyright 2020 Grant Likely <grant.likely@secretlab.ca>
 *
 * SPDX-License-Identifier: GPLv2
 *
 * Module initialization and utility routines for OBS
 */

#ifndef PTZ_H
#define PTZ_H

#include <obs-module.h>

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

extern obs_data_array_t *ptz_devices_get_config(void);
extern obs_source_t *ptz_device_get_parent_source(uint32_t device_id);
extern void ptz_devices_set_config(obs_data_array_t *devices);

/* Driver factory / teardown, dispatching by config["type"] / device_id.
 * The one place PTZListModel and settings.cpp need to reach an actual
 * PTZDevice subclass -- see ptz_device_create()'s comment in ptz-device.cpp. */
extern void ptz_device_create(obs_data_t *config);
extern void ptz_device_destroy(uint32_t device_id);

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

extern bool ptz_scene_is_source_active(obs_source_t *scene, obs_source_t *source);

extern proc_handler_t *ptz_get_proc_handler();
extern signal_handler_t *ptz_get_signal_handler();

/* The version of the PTZ API that ptz_get_api_version reports, on OBS's
 * proc_handler and on each of this plugin's devices: the procs and
 * signals in docs/ptz-device-api.md, and the keys and names they take. Bump
 * the minor version for a change an existing caller can't notice (something
 * added), and the major version, resetting the minor, for one it can
 * (something removed, renamed or changing meaning). Then update the doc,
 * which states it. */
#define PTZ_API_VERSION_MAJOR 0
#define PTZ_API_VERSION_MINOR 1

#ifdef __cplusplus
}
#endif

#endif /* PTZ_H */
