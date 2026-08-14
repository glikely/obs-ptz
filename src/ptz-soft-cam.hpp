/* Pan Tilt Zoom Controls - a soft PTZ camera: a viewport on any source
 *
 * Copyright 2026 Grant Likely <grant.likely@secretlab.ca>
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#pragma once

#include <mutex>
#include <QTimer>
#include <obs.h>
#include "ptz-device.hpp"
#include "ptz-soft-viewport.hpp"

/* A "camera" with no hardware: it pans, tilts and zooms by showing a part of
 * the source its filter is on, scaled up to fill the source's own size. It is
 * driven like any camera, and it moves like one (see SoftViewport).
 *
 * The device is on the Qt thread, and its filter's video_tick and
 * video_render are on OBS's graphics thread, so what they share, the
 * viewport, is behind m_lock. */
class PTZSoftCam : public PTZDevice {
	Q_OBJECT

private:
	mutable std::mutex m_lock;
	SoftViewport m_viewport;
	/* Where the viewport is, for the device's state, a few times a second */
	QTimer m_reportTimer;
	void reportPosition();
	/* Where it was when it was last written to the settings */
	SoftPosition m_persisted;
	/* What the picture is drawn to, before it is drawn to the scene, so that
	 * the zoomed picture cannot be drawn outside the source's own size. Only
	 * the graphics thread, and the destructor, touch it. */
	gs_texrender_t *m_texrender = nullptr;

public:
	PTZSoftCam(OBSData config, obs_source_t *filter);
	~PTZSoftCam();

	static void defaults(obs_data_t *settings);
	void update(OBSData config) override;
	void save(OBSData config) const override;
	/* Where it is looking, which is the device's own and so not a setting */
	void persistState(obs_data_t *settings) const override;
	obs_properties_t *get_obs_properties() override;

	Features features() const override;
	void do_update() override;
	void pantilt_abs(double pan, double tilt) override;
	void pantilt_rel(double pan, double tilt) override;
	void pantilt_home() override;
	void zoom_abs(double pos) override;

	/* For the filter's callbacks, on the graphics thread */
	void tickViewport(double seconds);
	SoftRect visibleRect() const;
	/* The texture the picture is drawn to, made when it is first wanted */
	gs_texrender_t *texrender();
};

void ptz_soft_cam_register_filter();
