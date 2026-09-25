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

extern bool ptz_scene_is_source_active(obs_source_t *scene, obs_source_t *source);

extern proc_handler_t *ptz_get_proc_handler();
extern signal_handler_t *ptz_get_signal_handler();

#ifdef __cplusplus
}
#endif

#endif /* PTZ_H */
