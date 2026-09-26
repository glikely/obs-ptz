/* Pan Tilt Zoom USB UVC implementation
	*
	* Copyright 2025 Fabio Ferrari <fabio.ferrar@gmail.com>
	*
	* SPDX-License-Identifier: GPLv2
	*/

#include <qt-wrappers.hpp>
#include "ptz-device.hpp"
#include <cstddef>
#include <obs-data.h>
#include <obs-properties.h>
#include <obs.h>
#include <obs.hpp>
#include "ptz-usb-cam.hpp"

void PTZUSBCam::ptz_tick_callback(void *param, float seconds)
{
	PTZUSBCam *cam = static_cast<PTZUSBCam *>(param);
	cam->ptz_tick(seconds);
}

PTZUSBCam::PTZUSBCam(OBSData config) : PTZDevice(config)
{
	getDefaults(config);
	update(config);
	obs_add_tick_callback(ptz_tick_callback, this);
}

PTZUSBCam::~PTZUSBCam()
{
	obs_remove_tick_callback(ptz_tick_callback, this);
}

QString PTZUSBCam::description()
{
	return QString(obs_module_text("PTZ.UVC.Name"));
}

void PTZUSBCam::update(OBSData config)
{
	PTZDevice::update(config);
	OBSDataArrayAutoRelease presetArray = obs_data_get_array(config, "presets_memory");
	size_t count = obs_data_array_count(presetArray);
	for (size_t i = 0; i < count; ++i) {
		OBSDataAutoRelease preset = obs_data_array_item(presetArray, i);
		int p_id = static_cast<int>(obs_data_get_int(preset, "preset_id"));
		PtzUsbCamPos p = PtzUsbCamPos();
		p.pan = obs_data_get_double(preset, "pan");
		p.tilt = obs_data_get_double(preset, "tilt");
		p.zoom = obs_data_get_double(preset, "zoom");
		p.focusAuto = obs_data_get_bool(preset, "focusauto");
		p.focus = obs_data_get_double(preset, "focus");
		// p.whitebalAuto = obs_data_get_bool(preset, "whitebalauto");
		// p.temperature = obs_data_get_double(preset, "temperature");
		presets[p_id] = p;
	}
}

void PTZUSBCam::save(OBSData config) const
{
	PTZDevice::save(config);
	OBSDataArrayAutoRelease presetArray = obs_data_array_create();
	for (auto it = presets.constBegin(); it != presets.constEnd(); ++it) {
		const PtzUsbCamPos &preset = it.value();
		OBSDataAutoRelease presetData = obs_data_create();
		obs_data_set_double(presetData, "preset_id", it.key());
		obs_data_set_double(presetData, "pan", preset.pan);
		obs_data_set_double(presetData, "tilt", preset.tilt);
		obs_data_set_double(presetData, "zoom", preset.zoom);
		obs_data_set_bool(presetData, "focusauto", preset.focusAuto);
		obs_data_set_double(presetData, "focus", preset.focus);
		// obs_data_set_bool(presetData, "whitebalauto",
		// 		  preset.whitebalAuto);
		// obs_data_set_double(presetData, "temperature", preset.temperature);
		obs_data_array_push_back(presetArray, presetData);
	}
	obs_data_set_array(config, "presets_memory", presetArray);
}

obs_properties_t *PTZUSBCam::get_obs_properties()
{
	obs_properties_t *ptz_props = PTZDevice::get_obs_properties();
	obs_properties_remove_by_name(ptz_props, "interface");
	return ptz_props;
}

void PTZUSBCam::do_update()
{
	pantilt_changed = false;
	zoom_changed = false;
	focus_changed = false;
}

PTZUsbBackend *PTZUSBCam::getBackend()
{
	std::string video_device_id = "";
	OBSSourceAutoRelease src = parentSource();
	if (src) {
		OBSDataAutoRelease psettings = obs_source_get_settings(src);
		if (psettings) {
			video_device_id = obs_data_get_string(psettings, ptz_usb_source_setting_key());
		}
	}

	if (video_device_id != backend_slot_.deviceId()) {
		blog(LOG_INFO, "Switching PTZ USBUVC device from %s to %s",
		     backend_slot_.deviceId().empty() ? "null" : backend_slot_.deviceId().c_str(),
		     video_device_id.empty() ? "null" : video_device_id.c_str());
	}
	return backend_slot_.get(video_device_id);
}

void PTZUSBCam::ptz_tick(float seconds)
{
	tick_elapsed += seconds;
	if (tick_elapsed < 0.03f)
		return;
	if (pan_speed != 0.0 || tilt_speed != 0.0) {
		pantilt_rel(pan_speed * tick_elapsed, tilt_speed * tick_elapsed);
	}

	auto ptzctrl = getBackend();
	setConnected(ptzctrl != nullptr);
	if (!ptzctrl)
		return;
	if (zoom_speed != 0.0)
		zoom_abs(ptzctrl->getZoom() + zoom_speed * tick_elapsed);
	if (focus_speed != 0.0)
		focus_abs(ptzctrl->getFocus() + focus_speed * tick_elapsed);
	tick_elapsed = 0.0f;
}

void PTZUSBCam::pantilt_abs(double pan, double tilt)
{
	auto ptzctrl = getBackend();
	if (!ptzctrl)
		return;
	ptzctrl->pan(pan);
	ptzctrl->tilt(tilt);
}

void PTZUSBCam::pantilt_rel(double pan, double tilt)
{
	auto ptzctrl = getBackend();
	if (!ptzctrl)
		return;
	pantilt_abs(ptzctrl->getPan() + pan, ptzctrl->getTilt() + tilt);
}

void PTZUSBCam::pantilt_home()
{
	pantilt_abs(0, 0);
}

void PTZUSBCam::zoom_abs(double pos)
{
	auto ptzctrl = getBackend();
	if (!ptzctrl)
		return;
	ptzctrl->zoom(pos);
}

void PTZUSBCam::focus_abs(double pos)
{
	auto ptzctrl = getBackend();
	if (!ptzctrl)
		return;
	ptzctrl->focus(pos);
}

void PTZUSBCam::set_autofocus(bool enabled)
{
	auto ptzctrl = getBackend();
	if (!ptzctrl)
		return;
	ptzctrl->setAutoFocus(enabled);
}

void PTZUSBCam::memory_reset(int i)
{
	if (!presets.contains(i))
		return;
	presets.remove(i);
}

void PTZUSBCam::memory_set(int i)
{
	auto ptzctrl = getBackend();
	if (!ptzctrl)
		return;
	presets[i] = ptzctrl->getPosition();
}

void PTZUSBCam::memory_recall(int i)
{
	if (!presets.contains(i))
		return;
	auto now_pos = presets[i];
	pantilt_abs(now_pos.pan, now_pos.tilt);
	zoom_abs(now_pos.zoom);
	set_autofocus(now_pos.focusAuto);
	if (!now_pos.focusAuto)
		focus_abs(now_pos.focus);
}
