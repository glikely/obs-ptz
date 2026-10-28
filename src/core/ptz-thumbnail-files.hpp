/* Pan Tilt Zoom Controls - preset thumbnail files
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
 * thumbnail is first written. The controller's ptz_thumbnail_sweep()
 * computes the same directory and must agree with it. */
QString ptz_thumbnail_dir();

/* Full path of a thumbnail image, given the file name kept in the preset */
QString ptz_thumbnail_path(const QString &fileName);

/* Renders one frame of the source into a QImage at most
 * PTZ_THUMBNAIL_WIDTH wide. The rendering happens on OBS's graphics thread
 * and the callback is then run on context's thread, and not at all if
 * context is destroyed first. The callback gets a null image if the source
 * has nothing to show. Safe to call from any thread. */
void ptz_capture_source_thumbnail(obs_source_t *source, QObject *context, std::function<void(QImage)> callback);

/* A new unique file name for a thumbnail of the filter whose uuid is `owner`,
 * with that suffix: "<owner>_<random uuid>.<suffix>". A name says who it is
 * for, so a filter OBS duplicated, which has a uuid of its own and its
 * source's presets, can tell which of their thumbnails it was given. An empty
 * owner gives a name with no owner, as every name was before. */
QString ptz_thumbnail_new_name(const QString &owner, const QString &suffix);

/* The uuid of the filter a thumbnail's file name says it is for, or "" if
 * the name says none */
QString ptz_thumbnail_owner(const QString &fileName);

/* Writes the image into the thumbnail directory under a new unique name for
 * the filter `owner` and returns that name, or an empty string if it could
 * not be written */
QString ptz_thumbnail_write(const QImage &image, const QString &owner);

/* Deletes a thumbnail file, by the name kept in the preset */
void ptz_thumbnail_remove(const QString &fileName);
