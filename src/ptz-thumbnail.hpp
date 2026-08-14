/* Pan Tilt Zoom Controls - preset thumbnails
 *
 * Copyright 2026 Grant Likely <grant.likely@secretlab.ca>
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#pragma once

#include <functional>
#include <obs.h>
#include <QImage>
#include <QObject>
#include <QString>

/* Width in pixels of a stored preset thumbnail; the height follows the
 * captured source's aspect ratio */
static constexpr int PTZ_THUMBNAIL_WIDTH = 192;

/* Directory the thumbnail image files live in. Only created when a
 * thumbnail is first written. */
QString ptz_thumbnail_dir();

/* Full path of a thumbnail image, given the file name kept in the preset */
QString ptz_thumbnail_path(const QString &fileName);

/* Renders one frame of the source into a QImage at most
 * PTZ_THUMBNAIL_WIDTH wide. The rendering happens on OBS's graphics thread
 * and the callback is then run on context's thread, and not at all if
 * context is destroyed first. The callback gets a null image if the source
 * has nothing to show. Safe to call from any thread. */
void ptz_capture_source_thumbnail(obs_source_t *source, QObject *context, std::function<void(QImage)> callback);

/* Writes the image into the thumbnail directory under a new unique name and
 * returns that name, or an empty string if it could not be written */
QString ptz_thumbnail_write(const QImage &image);

/* Deletes a thumbnail file, by the name kept in the preset */
void ptz_thumbnail_remove(const QString &fileName);

/* Deletes thumbnail files that no preset refers to, left behind by importing
 * presets, removing a device, or a crash. The thumbnails are shared by every
 * scene collection, so a file counts as used if the name appears in any of
 * OBS's scene collection files or in this plugin's config.json, not just in
 * the devices loaded now. If any of those can't be read, nothing is
 * deleted. Files changed within the last day are left alone. Runs in the
 * background. */
void ptz_thumbnail_sweep();
