/* Pan Tilt Zoom Controls - sweeping unused preset thumbnails
 *
 * Copyright 2026 Grant Likely <grant.likely@secretlab.ca>
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include <obs-module.h>
#include <QByteArray>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QThreadPool>
#include "ptz-thumbnail-sweep.hpp"

/* The directory the devices write thumbnails in. It must be the same as
 * ptz_thumbnail_dir() in ptz-thumbnail-files.cpp, which the devices use. */
static QString thumbnailDirectory()
{
	char *path = obs_module_config_path("thumbnails");
	if (!path)
		return QString();
	QString dir = QString::fromUtf8(path);
	bfree(path);
	return dir;
}

/* Runs on a worker thread */
static void sweepThumbnails(QString thumbnailDir, QString configDir)
{
	QDir thumbnails(thumbnailDir);
	if (!thumbnails.exists())
		return;

	/* Everything that could name a thumbnail: the scene collections, which
	 * hold every filter's settings, and our own config for the devices that
	 * aren't filters. The names are UUIDs, so searching the raw text for
	 * them is enough, and needs no knowledge of the files' layout. */
	QDir scenes(QDir(configDir).filePath("../../basic/scenes"));
	QFileInfoList sources = scenes.entryInfoList({"*.json*"}, QDir::Files);
	if (!scenes.exists() || sources.isEmpty()) {
		blog(LOG_INFO, "[obs-ptz] thumbnail sweep skipped: can't find the scene collections");
		return;
	}
	sources.append(QFileInfo(QDir(configDir).filePath("config.json")));

	QByteArray text;
	for (const QFileInfo &info : sources) {
		QFile file(info.filePath());
		if (!file.exists() && info.fileName() == "config.json")
			continue;
		if (!file.open(QIODevice::ReadOnly)) {
			blog(LOG_INFO, "[obs-ptz] thumbnail sweep skipped: can't read %s",
			     qUtf8Printable(info.filePath()));
			return;
		}
		text += file.readAll();
	}

	QDateTime recent = QDateTime::currentDateTime().addDays(-1);
	int removed = 0;
	for (const QFileInfo &info : thumbnails.entryInfoList(QDir::Files)) {
		if (info.lastModified() > recent || text.contains(info.fileName().toUtf8()))
			continue;
		if (QFile::remove(info.filePath()))
			removed++;
	}
	if (removed)
		blog(LOG_INFO, "[obs-ptz] removed %d unused preset thumbnails", removed);
}

void ptz_thumbnail_sweep()
{
	QString thumbnailDir = thumbnailDirectory();
	char *config = obs_module_config_path("");
	if (thumbnailDir.isEmpty() || !config)
		return;
	QString configDir = QString::fromUtf8(config);
	bfree(config);
	QThreadPool::globalInstance()->start([=] { sweepThumbnails(thumbnailDir, configDir); });
}
