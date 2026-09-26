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

PTZUSBCam::PTZUSBCam(OBSData config) : PTZDevice(config), worker_(new PTZUsbWorker(ptz_usb_backend_create))
{
	getDefaults(config);
	update(config);

	/* The worker's thread reports back through queued signals, which are
	 * delivered here on the device's thread. */
	connect(worker_.get(), &PTZUsbWorker::connectedChanged, this,
		[this](bool connected) { setConnected(connected); });
	connect(worker_.get(), &PTZUsbWorker::positionCaptured, this,
		[this](int id, double pan, double tilt, double zoom, bool focusAuto, double focus) {
			PtzUsbCamPos pos;
			pos.pan = pan;
			pos.tilt = tilt;
			pos.zoom = zoom;
			pos.focusAuto = focusAuto;
			pos.focus = focus;
			presets[id] = pos;
		});

	connect(&device_id_timer_, &QTimer::timeout, this, &PTZUSBCam::refreshDeviceId);
	device_id_timer_.start(250);
	refreshDeviceId();
}

PTZUSBCam::~PTZUSBCam()
{
	device_id_timer_.stop();
	/* Waits for the worker's thread to finish, and closes the camera */
	worker_.reset();
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
	worker_->setSpeeds(pan_speed, tilt_speed, zoom_speed, focus_speed);
	pantilt_changed = false;
	zoom_changed = false;
	focus_changed = false;
}

/* Find out which camera the source is set to, and tell the worker if that's
 * changed. Nothing here talks to the camera, so it is fine to do on the
 * device's thread, and before every command so none goes to a stale camera. */
void PTZUSBCam::refreshDeviceId()
{
	std::string video_device_id = "";
	OBSSourceAutoRelease src = parentSource();
	if (src) {
		OBSDataAutoRelease psettings = obs_source_get_settings(src);
		if (psettings) {
			video_device_id = obs_data_get_string(psettings, ptz_usb_source_setting_key());
		}
	}

	if (video_device_id == device_id_)
		return;
	blog(LOG_INFO, "Switching PTZ USBUVC device from %s to %s", device_id_.empty() ? "null" : device_id_.c_str(),
	     video_device_id.empty() ? "null" : video_device_id.c_str());
	device_id_ = video_device_id;
	worker_->setDeviceId(device_id_);
}

void PTZUSBCam::pantilt_abs(double pan, double tilt)
{
	refreshDeviceId();
	worker_->pantiltAbs(pan, tilt);
}

void PTZUSBCam::pantilt_rel(double pan, double tilt)
{
	refreshDeviceId();
	worker_->pantiltRel(pan, tilt);
}

void PTZUSBCam::pantilt_home()
{
	pantilt_abs(0, 0);
}

void PTZUSBCam::zoom_abs(double pos)
{
	refreshDeviceId();
	worker_->zoomAbs(pos);
}

void PTZUSBCam::focus_abs(double pos)
{
	refreshDeviceId();
	worker_->focusAbs(pos);
}

void PTZUSBCam::set_autofocus(bool enabled)
{
	refreshDeviceId();
	worker_->setAutoFocus(enabled);
}

void PTZUSBCam::memory_reset(int i)
{
	if (!presets.contains(i))
		return;
	presets.remove(i);
}

void PTZUSBCam::memory_set(int i)
{
	/* Answered by the positionCaptured signal, once the worker has got to it */
	refreshDeviceId();
	worker_->capturePosition(i);
}

void PTZUSBCam::memory_recall(int i)
{
	if (!presets.contains(i))
		return;
	refreshDeviceId();
	worker_->recall(presets[i]);
}
