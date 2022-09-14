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
#include "plugin-macros.generated.h"

#define STRINGIFY(x) #x
#define TOSTRING(x) STRINGIFY(x)

#ifdef __cplusplus
extern "C" {
#endif

extern void ptz_load_devices(void);
extern void ptz_unload_devices(void);
extern void ptz_load_action_source(void);
extern void ptz_load_controls(void);
extern void ptz_load_settings(void);

extern obs_data_array_t *ptz_devices_get_config(void);
extern void ptz_devices_set_config(obs_data_array_t *devices);

extern bool ptz_scene_is_source_active(obs_source_t *scene,
				       obs_source_t *source);

extern proc_handler_t *ptz_get_proc_handler();

#ifdef __cplusplus
}
#endif

#endif /* PTZ_H */
