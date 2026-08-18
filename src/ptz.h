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

extern obs_source_t *ptz_device_find_source_using_ptz_name(uint32_t device_id);
/* Live enumeration, not persistence -- see the comment on the definition in
 * ptz-device.cpp. Used by ptz-action-source.c's properties dialog. */
extern obs_data_array_t *ptz_devices_get_config(void);

extern bool ptz_scene_is_source_active(obs_source_t *scene, obs_source_t *source);

extern proc_handler_t *ptz_get_proc_handler();
extern signal_handler_t *ptz_get_signal_handler();

#ifdef __cplusplus
}

/* C++-only: PTZDevice is a C++ class, not something ptz.c (a plain C file
 * that also includes this header) can name, so this can't live in the
 * extern "C" block above. The driver factory, dispatching by
 * config["type"] -- the one place the PTZ Control filter (ptz-device.cpp)
 * needs to reach an actual PTZDevice subclass; see the comment on its
 * definition. */
class PTZDevice;
extern PTZDevice *ptz_device_create(obs_data_t *config, obs_source_t *filter_source);
#endif

#endif /* PTZ_H */
