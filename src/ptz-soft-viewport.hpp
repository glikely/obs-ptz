/* Pan Tilt Zoom Controls - the viewport of a soft PTZ camera
 *
 * Copyright 2026 Grant Likely <grant.likely@secretlab.ca>
 *
 * SPDX-License-Identifier: GPLv2
 */
#pragma once

#include <algorithm>
#include <cmath>

/* No Qt and no OBS in here, so that it is tested on its own, see
 * tests/soft-ptz. Nothing is thread-safe: the caller's lock is. */

struct SoftViewportConfig {
	/* The scale at zoom 1.0: [1.1, 10.0] */
	double maxZoom = 4.0;
	/* Pan and tilt units a second at full speed and scale 1 */
	double panTiltRate = 0.5;
	/* Zoom units a second at full speed */
	double zoomRate = 0.5;
	/* How long an eased move takes */
	double recallSeconds = 1.0;
};

/* Where the viewport is, in the units of the PTZ API: pan and tilt in
 * [-1, 1] (positive is right and up), zoom in [0, 1] (0 is the whole frame) */
struct SoftPosition {
	double pan = 0;
	double tilt = 0;
	double zoom = 0;
};

/* The part of the frame that is shown, as fractions of it, from its top left */
struct SoftRect {
	double x = 0, y = 0, w = 1, h = 1;
};

/* Pan and tilt are where the viewport's centre is, in frame units where +-1
 * is the edge of the frame, kept so that the viewport never leaves it: at
 * scale s no further than 1 - 1/s. What is commanded is kept as it was
 * commanded and clamped when read, so that a position given before the zoom
 * that makes room for it (as a preset recall does) is not lost. */
class SoftViewport {
public:
	void setConfig(const SoftViewportConfig &config)
	{
		m_config = config;
		m_config.maxZoom = std::clamp(finiteOr(m_config.maxZoom, 4.0), 1.1, 10.0);
		m_config.panTiltRate = std::max(0.0, finiteOr(m_config.panTiltRate, 0.5));
		m_config.zoomRate = std::max(0.0, finiteOr(m_config.zoomRate, 0.5));
		m_config.recallSeconds = std::max(0.0, finiteOr(m_config.recallSeconds, 1.0));
		/* What is commanded may have been out of the new range's reach */
		SoftPosition now = position();
		m_pos = now;
	}
	const SoftViewportConfig &config() const { return m_config; }

	/* How much the frame is magnified: maxZoom ^ zoom */
	double scale() const { return std::pow(m_config.maxZoom, clamp01(m_pos.zoom)); }

	/* Where the viewport is, kept inside the frame: what is reported */
	SoftPosition position() const
	{
		double limit = limitAt(scale());
		return {std::clamp(m_pos.pan, -limit, limit), std::clamp(m_pos.tilt, -limit, limit),
			clamp01(m_pos.zoom)};
	}

	SoftRect visibleRect() const
	{
		SoftPosition p = position();
		double size = 1.0 / scale();
		/* A pan unit is half a frame; the frame's y runs down, tilt runs up */
		return {0.5 + p.pan * 0.5 - size / 2, 0.5 - p.tilt * 0.5 - size / 2, size, size};
	}

	/* Go there at once. Anything that is not a position is ignored. */
	void jumpTo(const SoftPosition &to)
	{
		if (!std::isfinite(to.pan) || !std::isfinite(to.tilt) || !std::isfinite(to.zoom))
			return;
		m_pos = {std::clamp(to.pan, -1.0, 1.0), std::clamp(to.tilt, -1.0, 1.0), clamp01(to.zoom)};
	}

private:
	SoftViewportConfig m_config;
	SoftPosition m_pos;

	static double limitAt(double scale) { return 1.0 - 1.0 / scale; }
	static double clamp01(double v) { return std::clamp(v, 0.0, 1.0); }
	static double finiteOr(double v, double fallback) { return std::isfinite(v) ? v : fallback; }
};
