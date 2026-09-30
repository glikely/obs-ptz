/* Pan Tilt Zoom Controls - preset thumbnails
 *
 * Copyright 2026 Grant Likely <grant.likely@secretlab.ca>
 *
 * SPDX-License-Identifier: GPLv2
 */

#include <obs-module.h>
#include <obs.hpp>
#include <graphics/graphics.h>
#include <graphics/vec4.h>
#include <memory>
#include <QDir>
#include <QFileInfo>
#include <QPointer>
#include <QDateTime>
#include <QFile>
#include <QThreadPool>
#include <QMetaObject>
#include <QUuid>
#include "ptz-thumbnail.hpp"

QString ptz_thumbnail_dir()
{
	char *path = obs_module_config_path("thumbnails");
	if (!path)
		return QString();
	QString dir = QString::fromUtf8(path);
	bfree(path);
	return dir;
}

QString ptz_thumbnail_path(const QString &fileName)
{
	if (fileName.isEmpty() || fileName != QFileInfo(fileName).fileName())
		return QString();
	QString dir = ptz_thumbnail_dir();
	return dir.isEmpty() ? QString() : QDir(dir).filePath(fileName);
}

QString ptz_thumbnail_write(const QImage &image)
{
	if (image.isNull())
		return QString();
	QDir().mkpath(ptz_thumbnail_dir());
	QString base = QUuid::createUuid().toString(QUuid::WithoutBraces);
	/* JPEG is much smaller, but not every Qt build ships the plugin */
	for (const char *ext : {"jpg", "png"}) {
		QString name = base + "." + ext;
		QString path = ptz_thumbnail_path(name);
		if (!path.isEmpty() && image.save(path, ext, 85))
			return name;
	}
	return QString();
}

void ptz_thumbnail_remove(const QString &fileName)
{
	QString path = ptz_thumbnail_path(fileName);
	if (!path.isEmpty())
		QFile::remove(path);
}

namespace {

struct CaptureJob {
	OBSWeakSource source;
	QPointer<QObject> context;
	std::function<void(QImage)> callback;
};

/* Runs on the graphics thread */
QImage renderSource(obs_source_t *source)
{
	uint32_t srcWidth = obs_source_get_base_width(source);
	uint32_t srcHeight = obs_source_get_base_height(source);
	if (!srcWidth || !srcHeight)
		return QImage();

	uint32_t width = qMin<uint32_t>(PTZ_THUMBNAIL_WIDTH, srcWidth);
	uint32_t height = qMax<uint32_t>(1, (uint64_t)width * srcHeight / srcWidth);

	QImage image;
	obs_enter_graphics();
	gs_texrender_t *texrender = gs_texrender_create(GS_BGRA, GS_ZS_NONE);
	gs_stagesurf_t *stage = gs_stagesurface_create(width, height, GS_BGRA);

	gs_texrender_reset(texrender);
	if (gs_texrender_begin(texrender, width, height)) {
		vec4 clear;
		vec4_zero(&clear);
		gs_clear(GS_CLEAR_COLOR, &clear, 0.0f, 0);
		gs_ortho(0.0f, (float)srcWidth, 0.0f, (float)srcHeight, -100.0f, 100.0f);

		gs_blend_state_push();
		gs_blend_function(GS_BLEND_ONE, GS_BLEND_ZERO);
		obs_source_inc_showing(source);
		obs_source_video_render(source);
		obs_source_dec_showing(source);
		gs_blend_state_pop();
		gs_texrender_end(texrender);

		gs_stage_texture(stage, gs_texrender_get_texture(texrender));
		uint8_t *data = nullptr;
		uint32_t linesize = 0;
		if (gs_stagesurface_map(stage, &data, &linesize)) {
			/* BGRA in memory is Format_ARGB32 on little-endian, which
			 * is every platform OBS runs on. Copy: the mapping goes away
			 * below. */
			image = QImage(data, width, height, linesize, QImage::Format_ARGB32)
					.convertToFormat(QImage::Format_RGB32);
			gs_stagesurface_unmap(stage);
		}
	}
	gs_stagesurface_destroy(stage);
	gs_texrender_destroy(texrender);
	obs_leave_graphics();
	return image;
}

void captureTask(void *param)
{
	std::unique_ptr<CaptureJob> job(static_cast<CaptureJob *>(param));
	QImage image;
	OBSSourceAutoRelease source = obs_weak_source_get_source(job->source);
	if (source)
		image = renderSource(source);

	QObject *context = job->context;
	if (!context)
		return;
	auto callback = std::move(job->callback);
	QMetaObject::invokeMethod(context, [callback, image] { callback(image); }, Qt::QueuedConnection);
}

} // namespace

void ptz_capture_source_thumbnail(obs_source_t *source, QObject *context, std::function<void(QImage)> callback)
{
	if (!source || !context || !callback)
		return;
	auto *job = new CaptureJob{OBSGetWeakRef(source), context, std::move(callback)};
	obs_queue_task(OBS_TASK_GRAPHICS, captureTask, job, false);
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
	QString thumbnailDir = ptz_thumbnail_dir();
	char *config = obs_module_config_path("");
	if (thumbnailDir.isEmpty() || !config)
		return;
	QString configDir = QString::fromUtf8(config);
	bfree(config);
	QThreadPool::globalInstance()->start([=] { sweepThumbnails(thumbnailDir, configDir); });
}
