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
/* Starts and stops the PTZ API's binding to obs-websocket (ptz-websocket.cpp).
 * To be started from obs_module_post_load(), when obs-websocket has loaded. */
extern void ptz_websocket_load(void);
extern void ptz_websocket_unload(void);

/* The "PTZ Control" filter kind that a device "type" of the old self-managed
 * backend, or of a backup that has no "kind", belongs to, or NULL if none.
 * Goes with the legacy migration (see ptz-legacy-migration.cpp). */
extern const char *ptz_legacy_kind_for_type(const char *type);
/* Marks `defaults`, in a filter kind's get_defaults, as those of a PTZ driver
 * (see "Driver kinds" in docs/ptz-device-api.md), by the version of the PTZ
 * API it implements. Call it first. */
extern void ptz_filter_defaults_mark(obs_data_t *defaults);
/* Adds a PTZ Control filter, and so a device, to `parent`, with the settings
 * in `config`, of the filter kind in its "kind" or, without one, of its
 * "type" (see ptz_legacy_kind_for_type()). Returns a new reference to the
 * filter, or NULL, as it does for a kind no module has registered. */
extern obs_source_t *ptz_device_create_filter(obs_source_t *parent, obs_data_t *config);

/* Whether a source is a device of the PTZ API, from this plugin or from any
 * other, a filter or not: its proc_handler answers ptz_get_api_version with
 * the same major version as this plugin's and a minor version at least
 * PTZ_API_VERSION_MINOR_NEEDED (see docs/ptz-device-api.md). Nothing about its id says so. */
extern bool ptz_source_is_device(obs_source_t *source);

/* Whether `source` is `scene` or one of the sources it shows. Inline, as both
 * the controller and the devices use it. */
struct ptz_source_active_data {
	obs_source_t *source;
	bool active;
};

static inline void ptz_source_active_cb(obs_source_t *parent, obs_source_t *child, void *data)
{
	struct ptz_source_active_data *cb_data = (struct ptz_source_active_data *)data;
	(void)parent;
	if (child == cb_data->source)
		cb_data->active = true;
}

static inline bool ptz_scene_is_source_active(obs_source_t *scene, obs_source_t *source)
{
	struct ptz_source_active_data cb_data;
	cb_data.source = source;
	cb_data.active = false;
	if (!scene || !source)
		return false;
	if (scene == source)
		return true;
	obs_source_enum_active_sources(scene, ptz_source_active_cb, &cb_data);
	return cb_data.active;
}

/* The version of the PTZ API that ptz_get_api_version reports, on each of this
 * plugin's devices (there is no version of the plugin as a whole): the procs and
 * signals in docs/ptz-device-api.md, and the keys and names they take. Bump
 * the minor version for a change an existing caller can't notice (something
 * added), and the major version, resetting the minor, for one it can
 * (something removed, renamed or changing meaning). Then update the doc,
 * which states it. */
#define PTZ_API_VERSION_MAJOR 0
#define PTZ_API_VERSION_MINOR 8
/* The minor version the controller needs of a device, of this plugin or of any
 * other: the newest it uses something of. It is not PTZ_API_VERSION_MINOR, which
 * goes up with what this plugin's devices offer, and would shut out a device
 * written for an older minor version that the controller can use as well as
 * ever. Nothing the controller uses has been added since 0.3. */
#define PTZ_API_VERSION_MINOR_NEEDED 3

/* The keys of the filter kind defaults that mark a PTZ driver, see
 * ptz_filter_defaults_mark() */
#define PTZ_DEFAULTS_MAJOR_KEY "ptz_api_version_major"
#define PTZ_DEFAULTS_MINOR_KEY "ptz_api_version_minor"

#ifdef __cplusplus
}
#endif

#endif /* PTZ_H */
