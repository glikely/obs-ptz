/* Pan Tilt Zoom Controls - a soft PTZ camera: a viewport on any source
 *
 * Copyright 2026 Grant Likely <grant.likely@secretlab.ca>
 *
 * SPDX-License-Identifier: GPLv2
 */

#include <algorithm>
#include <obs-module.h>
#include "ptz-soft-cam.hpp"

/* What a setting is limited to, in properties and in update() both */
static constexpr double MaxZoomMin = 1.1, MaxZoomMax = 10.0;
static constexpr double RateMin = 0.05, RateMax = 2.0;
static constexpr double RecallMax = 5.0;

PTZSoftCam::PTZSoftCam(OBSData config, obs_source_t *filter) : PTZDevice(config, filter)
{
	type = "soft-ptz";
	update(config);

	/* Where it was left: only now, not in update(), which a change of
	 * another setting calls with a pose that may be out of date */
	OBSDataAutoRelease pose = obs_data_get_obj(config, "viewport");
	if (pose) {
		SoftPosition saved{obs_data_get_double(pose, "pan"), obs_data_get_double(pose, "tilt"),
				   obs_data_get_double(pose, "zoom")};
		std::lock_guard<std::mutex> lock(m_lock);
		m_viewport.jumpTo(saved);
		m_persisted = m_viewport.position();
	}

	/* There is no link to lose, so it is never shown as disconnected */
	setConnected(true);

	connect(&m_reportTimer, &QTimer::timeout, this, &PTZSoftCam::reportPosition);
	m_reportTimer.start(66);
	reportPosition();
}

PTZSoftCam::~PTZSoftCam()
{
	m_reportTimer.stop();
}

void PTZSoftCam::defaults(obs_data_t *settings)
{
	PTZDevice::defaults(settings);
	obs_data_set_default_double(settings, "max_zoom", 4.0);
	obs_data_set_default_double(settings, "pantilt_rate", 0.5);
	obs_data_set_default_double(settings, "zoom_rate", 0.5);
	obs_data_set_default_double(settings, "recall_seconds", 1.0);
}

void PTZSoftCam::update(OBSData config)
{
	PTZDevice::update(config);

	SoftViewportConfig viewport;
	viewport.maxZoom = std::clamp(obs_data_get_double(config, "max_zoom"), MaxZoomMin, MaxZoomMax);
	viewport.panTiltRate = std::clamp(obs_data_get_double(config, "pantilt_rate"), RateMin, RateMax);
	viewport.zoomRate = std::clamp(obs_data_get_double(config, "zoom_rate"), RateMin, RateMax);
	viewport.recallSeconds = std::clamp(obs_data_get_double(config, "recall_seconds"), 0.0, RecallMax);
	std::lock_guard<std::mutex> lock(m_lock);
	m_viewport.setConfig(viewport);
}

void PTZSoftCam::save(OBSData config) const
{
	PTZDevice::save(config);
	std::lock_guard<std::mutex> lock(m_lock);
	const SoftViewportConfig &viewport = m_viewport.config();
	obs_data_set_double(config, "max_zoom", viewport.maxZoom);
	obs_data_set_double(config, "pantilt_rate", viewport.panTiltRate);
	obs_data_set_double(config, "zoom_rate", viewport.zoomRate);
	obs_data_set_double(config, "recall_seconds", viewport.recallSeconds);
}

void PTZSoftCam::persistState(obs_data_t *settings) const
{
	PTZDevice::persistState(settings);
	SoftPosition pos;
	{
		std::lock_guard<std::mutex> lock(m_lock);
		pos = m_viewport.position();
	}
	OBSDataAutoRelease pose = obs_data_create();
	obs_data_set_double(pose, "pan", pos.pan);
	obs_data_set_double(pose, "tilt", pos.tilt);
	obs_data_set_double(pose, "zoom", pos.zoom);
	obs_data_set_obj(settings, "viewport", pose);
}

obs_properties_t *PTZSoftCam::get_obs_properties()
{
	obs_properties_t *props = PTZDevice::get_obs_properties();
	/* There is no connection to set up */
	obs_properties_remove_by_name(props, "interface");

	obs_properties_t *general = obs_property_group_content(obs_properties_get(props, "general"));
	obs_properties_add_float_slider(general, "max_zoom", obs_module_text("PTZ.Device.SoftPtz.MaxZoom"), MaxZoomMin,
					MaxZoomMax, 0.1);
	obs_properties_add_float_slider(general, "pantilt_rate", obs_module_text("PTZ.Device.SoftPtz.PanTiltRate"),
					RateMin, RateMax, 0.05);
	obs_properties_add_float_slider(general, "zoom_rate", obs_module_text("PTZ.Device.SoftPtz.ZoomRate"), RateMin,
					RateMax, 0.05);
	obs_properties_add_float_slider(general, "recall_seconds", obs_module_text("PTZ.Device.SoftPtz.RecallSeconds"),
					0.0, RecallMax, 0.05);
	return props;
}

/* Moves to a position, and by a distance, with the speed commands and Home;
 * no focus, and no presets of its own, the local ones follow from the
 * absolute positions */
PTZDevice::Features PTZSoftCam::features() const
{
	return PanTilt | Zoom | PanTiltAbs | PanTiltRel | ZoomAbs | Home;
}

void PTZSoftCam::do_update()
{
	{
		std::lock_guard<std::mutex> lock(m_lock);
		m_viewport.setSpeeds(pan_speed, tilt_speed, zoom_speed);
	}
	pantilt_changed = false;
	zoom_changed = false;
	focus_changed = false;
}

void PTZSoftCam::pantilt_abs(double pan, double tilt)
{
	std::lock_guard<std::mutex> lock(m_lock);
	m_viewport.movePanTiltTo(pan, tilt);
}

void PTZSoftCam::pantilt_rel(double pan, double tilt)
{
	std::lock_guard<std::mutex> lock(m_lock);
	m_viewport.moveBy(pan, tilt);
}

void PTZSoftCam::pantilt_home()
{
	std::lock_guard<std::mutex> lock(m_lock);
	m_viewport.home();
}

void PTZSoftCam::zoom_abs(double pos)
{
	std::lock_guard<std::mutex> lock(m_lock);
	m_viewport.moveZoomTo(pos);
}

void PTZSoftCam::tickViewport(double seconds)
{
	std::lock_guard<std::mutex> lock(m_lock);
	m_viewport.tick(seconds);
}

SoftRect PTZSoftCam::visibleRect() const
{
	std::lock_guard<std::mutex> lock(m_lock);
	return m_viewport.visibleRect();
}

void PTZSoftCam::reportPosition()
{
	SoftPosition pos;
	bool moving;
	{
		std::lock_guard<std::mutex> lock(m_lock);
		pos = m_viewport.position();
		moving = m_viewport.moving();
	}
	/* Once it has settled, where it is goes in the settings, so that it is
	 * there again when OBS is */
	if (!moving && (pos.pan != m_persisted.pan || pos.tilt != m_persisted.tilt || pos.zoom != m_persisted.zoom)) {
		m_persisted = pos;
		persist();
	}
	bool changed = false;
	changed |= setPosition("pan", pos.pan);
	changed |= setPosition("tilt", pos.tilt);
	changed |= setPosition("zoom", pos.zoom);
	if (changed)
		notifyStateChanged();
}

void ptz_soft_cam_register_filter()
{
	struct obs_source_info info = {};
	info.id = "ca.secretlab.obs-ptz.soft-ptz";
	info.type = OBS_SOURCE_TYPE_FILTER;
	info.output_flags = OBS_SOURCE_VIDEO | OBS_SOURCE_DO_NOT_DUPLICATE;
	info.get_name = [](void *) -> const char * {
		return "Soft PTZ Control";
	};
	info.create = [](obs_data_t *settings, obs_source_t *source) -> void * {
		return ptz_filter_create([&]() -> PTZDevice * { return new PTZSoftCam(settings, source); });
	};
	info.destroy = ptz_filter_destroy;
	info.get_defaults = [](obs_data_t *settings) {
		PTZSoftCam::defaults(settings);
	};
	info.get_properties = ptz_filter_get_properties;
	info.update = ptz_filter_update;
	info.save = ptz_filter_save;
	info.filter_remove = ptz_filter_remove;
	info.icon_type = OBS_ICON_TYPE_CAMERA;
	info.filter_add = ptz_filter_add;
	obs_register_source(&info);
}
