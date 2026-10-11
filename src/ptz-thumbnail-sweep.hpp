/* Pan Tilt Zoom Controls - sweeping unused preset thumbnails
 *
 * Copyright 2026 Grant Likely <grant.likely@secretlab.ca>
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#pragma once

/* Deletes thumbnail files that no preset refers to, left behind by importing
 * presets, removing a device, or a crash. The thumbnails are shared by every
 * scene collection, so a file counts as used if the name appears in any of
 * OBS's scene collection files or in this plugin's config.json, not just in
 * the devices loaded now. If any of those can't be read, nothing is
 * deleted. Files changed within the last day are left alone. Runs in the
 * background. */
void ptz_thumbnail_sweep();
