/* Pan Tilt Zoom Controls - migrate devices saved by the old self-managed backend
 *
 * Copyright 2026 Grant Likely <grant.likely@secretlab.ca>
 *
 * SPDX-License-Identifier: GPLv2
 */
#pragma once

#include <obs.h>
#include <stdbool.h>
#include <stdint.h>

/* TODO: remove this whole file, and the "devices" key it keeps in config.json,
 * two releases after the one that introduced it. By then anybody who is going
 * to upgrade has done so, and config.json.pre-filter-migration is their way
 * back. */

/* Takes the "devices" a config.json of the old self-managed backend held.
 * `file` is the config.json they were read from: it is copied to
 * `file`.pre-filter-migration, once, before anything is changed. */
void ptz_legacy_load(const char *file, obs_data_t *config);

/* The entries still waiting to be migrated, to be saved as "devices" in
 * config.json. Returns a new reference. */
obs_data_array_t *ptz_legacy_devices_save(void);

/* Makes a PTZ Control filter of each entry that belongs to the current scene
 * collection: on its source if the collection has one, otherwise a hidden
 * placeholder source in the current scene (also for an entry that has no
 * source, or whose source no collection has). Action sources that named the
 * old device by its id name its filter by UUID instead. Returns true if anything was changed, which
 * the caller should save. `finishing_loading` is true when OBS has just
 * finished loading: the devices made then are told so, as the ones that were
 * already there were. */
bool ptz_legacy_migrate(bool finishing_loading);

/* The UUID of the filter of the device that took the place of the self-managed
 * device `old_id`, or NULL if there is none (yet). Good until the next call
 * of ptz_legacy_migrate(). */
const char *ptz_legacy_remap_id(uint32_t old_id);
