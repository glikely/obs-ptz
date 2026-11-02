/* Pan Tilt Zoom Controls - the viewport of a virtual PTZ camera
 *
 * Copyright 2026 Grant Likely <grant.likely@secretlab.ca>
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#pragma once

#include <algorithm>
#include <cmath>

/* No Qt and no OBS in here, so that it is tested on its own, see
 * tests/virtual-ptz. Nothing is thread-safe: the caller's lock is. */

struct VirtualViewportConfig {
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
struct VirtualPosition {
	double pan = 0;
	double tilt = 0;
	double zoom = 0;
};

/* The part of the frame that is shown, as fractions of it, from its top left */
struct VirtualRect {
	double x = 0, y = 0, w = 1, h = 1;
};

/* Pan and tilt are where the viewport's centre is, in frame units where +-1
 * is the edge of the frame, kept so that the viewport never leaves it: at
 * scale s no further than 1 - 1/s. What is commanded is kept as it was
 * commanded and clamped when read, so that a position given before the zoom
 * that makes room for it (as a preset recall does) is not lost. */
class VirtualViewport {
public:
	void setConfig(const VirtualViewportConfig &config)
	{
		m_config = config;
		m_config.maxZoom = std::clamp(finiteOr(m_config.maxZoom, 4.0), 1.1, 10.0);
		m_config.panTiltRate = std::max(0.0, finiteOr(m_config.panTiltRate, 0.5));
		m_config.zoomRate = std::max(0.0, finiteOr(m_config.zoomRate, 0.5));
		m_config.recallSeconds = std::max(0.0, finiteOr(m_config.recallSeconds, 1.0));
		/* What is commanded may have been out of the new range's reach */
		VirtualPosition now = position();
		m_pos = now;
	}
	const VirtualViewportConfig &config() const { return m_config; }

	/* How much the frame is magnified: maxZoom ^ zoom */
	double scale() const { return std::pow(m_config.maxZoom, clamp01(m_pos.zoom)); }

	/* Where the viewport is, kept inside the frame: what is reported */
	VirtualPosition position() const
	{
		double limit = limitAt(scale());
		return {std::clamp(m_pos.pan, -limit, limit), std::clamp(m_pos.tilt, -limit, limit),
			clamp01(m_pos.zoom)};
	}

	VirtualRect visibleRect() const
	{
		VirtualPosition p = position();
		double size = 1.0 / scale();
		/* A pan unit is half a frame; the frame's y runs down, tilt runs up */
		return {0.5 + p.pan * 0.5 - size / 2, 0.5 - p.tilt * 0.5 - size / 2, size, size};
	}

	/* Go there at once. Anything that is not a position is ignored. */
	void jumpTo(const VirtualPosition &to)
	{
		if (!std::isfinite(to.pan) || !std::isfinite(to.tilt) || !std::isfinite(to.zoom))
			return;
		m_pos = {std::clamp(to.pan, -1.0, 1.0), std::clamp(to.tilt, -1.0, 1.0), clamp01(to.zoom)};
	}

	/* Moving at a speed in [-1, 1] on each axis, which goes on until it is
	 * changed. A speed ends an eased move; a speed of nothing does not. */
	void setSpeeds(double pan, double tilt, double zoom)
	{
		if (!std::isfinite(pan) || !std::isfinite(tilt) || !std::isfinite(zoom))
			return;
		m_speed = {std::clamp(pan, -1.0, 1.0), std::clamp(tilt, -1.0, 1.0), std::clamp(zoom, -1.0, 1.0)};
		if (m_speed.pan != 0 || m_speed.tilt != 0 || m_speed.zoom != 0)
			m_easing = false;
	}
	/* The speeds end, and an eased move goes on */
	void stop() { m_speed = {}; }

	/* Eased moves: from where the viewport is shown to there, over the
	 * recall time, ending the speeds. The axes a call does not name keep
	 * the target of the move they are in, so that pan and tilt, then zoom,
	 * as a preset recall asks for them, arrive together; a new call starts
	 * the time again from where the viewport is. */
	void moveTo(const VirtualPosition &target) { ease(target); }
	void movePanTiltTo(double pan, double tilt)
	{
		VirtualPosition target = easeTarget();
		target.pan = pan;
		target.tilt = tilt;
		ease(target);
	}
	void moveZoomTo(double zoom)
	{
		VirtualPosition target = easeTarget();
		target.zoom = zoom;
		ease(target);
	}
	/* By a distance, from where the viewport is going if it is going
	 * somewhere, so that nudges that come quicker than a move takes add up,
	 * and otherwise from where it is shown */
	void moveBy(double pan, double tilt)
	{
		if (!std::isfinite(pan) || !std::isfinite(tilt))
			return;
		VirtualPosition target = easeTarget();
		double limit = limitAt(std::pow(m_config.maxZoom, clamp01(target.zoom)));
		target.pan = std::clamp(target.pan, -limit, limit) + pan;
		target.tilt = std::clamp(target.tilt, -limit, limit) + tilt;
		ease(target);
	}
	void home() { moveTo({0, 0, 0}); }

	/* Advance by `seconds`. A speed's step is never more than a tenth of a
	 * second's: a frame that stalled is not a jump. An eased move is by the
	 * clock, so that after a stall it is as far along as it should be. */
	void tick(double seconds)
	{
		if (!std::isfinite(seconds) || seconds <= 0)
			return;
		if (m_easing) {
			m_elapsed += seconds;
			double t = m_config.recallSeconds > 0 ? std::min(1.0, m_elapsed / m_config.recallSeconds) : 1.0;
			if (t >= 1.0) {
				m_pos = m_to;
				m_easing = false;
			} else {
				double e = t * t * (3.0 - 2.0 * t);
				m_pos.pan = m_from.pan + (m_to.pan - m_from.pan) * e;
				m_pos.tilt = m_from.tilt + (m_to.tilt - m_from.tilt) * e;
				m_pos.zoom = m_from.zoom + (m_to.zoom - m_from.zoom) * e;
			}
			return;
		}
		if (m_speed.pan == 0 && m_speed.tilt == 0 && m_speed.zoom == 0)
			return;
		double dt = std::min(seconds, 0.1);
		m_pos.zoom = clamp01(m_pos.zoom + m_speed.zoom * m_config.zoomRate * dt);
		/* Over the scale: how far the picture moves is what the speed says */
		double step = m_config.panTiltRate * dt / scale();
		double limit = limitAt(scale());
		m_pos.pan = std::clamp(m_pos.pan + m_speed.pan * step, -limit, limit);
		m_pos.tilt = std::clamp(m_pos.tilt + m_speed.tilt * step, -limit, limit);
	}

	/* Going somewhere, or at a speed */
	bool moving() const { return m_easing || m_speed.pan != 0 || m_speed.tilt != 0 || m_speed.zoom != 0; }

private:
	VirtualViewportConfig m_config;
	/* Where it was commanded to be, which position() keeps in the frame */
	VirtualPosition m_pos;
	VirtualPosition m_speed;
	bool m_easing = false;
	VirtualPosition m_from, m_to;
	double m_elapsed = 0;

	/* What an eased move in progress is going to, or where the viewport was
	 * told to be: position() would lose a pan that waits for the zoom that
	 * makes room for it, as an instant recall sends them one after the other */
	VirtualPosition easeTarget() const { return m_easing ? m_to : m_pos; }
	void ease(VirtualPosition target)
	{
		if (!std::isfinite(target.pan) || !std::isfinite(target.tilt) || !std::isfinite(target.zoom))
			return;
		target = {std::clamp(target.pan, -1.0, 1.0), std::clamp(target.tilt, -1.0, 1.0), clamp01(target.zoom)};
		m_speed = {};
		if (m_config.recallSeconds <= 0) {
			m_pos = target;
			m_easing = false;
			return;
		}
		m_from = position();
		m_to = target;
		m_elapsed = 0;
		m_easing = true;
	}

	static double limitAt(double scale) { return 1.0 - 1.0 / scale; }
	static double clamp01(double v) { return std::clamp(v, 0.0, 1.0); }
	static double finiteOr(double v, double fallback) { return std::isfinite(v) ? v : fallback; }
};
