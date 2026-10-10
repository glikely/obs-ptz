/* Pan Tilt Zoom Controls - rolling backup of devices that have gone away
 *
 * Copyright 2026 Grant Likely <grant.likely@secretlab.ca>
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#pragma once

#include <obs.h>

/* Start and stop observing OBS for PTZ filters going away: a filter's
 * removal or destruction, and the removal of a source that has them. Each is
 * recorded in a rolling backup, so that a device lost by deleting its source
 * (which takes its filter, and with it the presets, along) can be added
 * again. Needs nothing from the devices. Stopping also forgets what was read
 * from the backup file, so the next start reads it again. */
void ptz_backup_store_start(void);
void ptz_backup_store_stop(void);

/* Settings of devices that were recently removed, most recent first: the
 * filter's settings, with the "name" of its source, the "kind" of its filter
 * and "backup_time" (in seconds since the epoch). At most one per name and
 * "type". Entries made before there was a "kind" have none. Returns a new
 * reference. */
obs_data_array_t *ptz_device_backups_get(void);
