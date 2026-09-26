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

PTZUSBCam::PTZUSBCam(OBSData config, obs_source_t *source)
	: PTZDevice(config, source),
	  worker_(new PTZUsbWorker(ptz_usb_backend_create))
{
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

	connect(worker_.get(), &PTZUsbWorker::stateCaptured, this,
		[this](PtzUsbCamPos pos, bool hasPan, bool hasTilt, bool hasZoom, bool hasFocus) {
			report_state(pos, hasPan, hasTilt, hasZoom, hasFocus);
		});

	connect(&device_id_timer_, &QTimer::timeout, this, &PTZUSBCam::refreshDeviceId);
	device_id_timer_.start(250);
	refreshDeviceId();

	/* Where the camera is, for the device's transient state: the position
	 * last sent to it (see PTZUsbWorker::captureState()), a few times a
	 * second is plenty */
	connect(&state_timer_, &QTimer::timeout, this, [this]() { worker_->captureState(); });
	state_timer_.start(100);
}

PTZUSBCam::~PTZUSBCam()
{
	state_timer_.stop();
	device_id_timer_.stop();
	/* Waits for the worker's thread to finish, and closes the camera */
	worker_.reset();
}

QString PTZUSBCam::description() const
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

/* pos's axes are meaningful only where the matching hasX is true -- an axis
 * the camera has no range for (a fixed-focus camera's focus, say) is left
 * out of the state entirely, rather than reported as a position of 0. */
void PTZUSBCam::report_state(PtzUsbCamPos pos, bool hasPan, bool hasTilt, bool hasZoom, bool hasFocus)
{
	bool changed = false;
	if (hasPan)
		changed |= setPosition("pan", pos.pan);
	if (hasTilt)
		changed |= setPosition("tilt", pos.tilt);
	if (hasZoom)
		changed |= setPosition("zoom", pos.zoom);
	if (hasFocus)
		changed |= setPosition("focus", pos.focus);
	if (changed)
		notifyStateChanged();
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

void ptz_usb_cam_register_filter()
{
	struct obs_source_info info = {};
	info.id = "ca.secretlab.obs-ptz.usb-cam";
	info.type = OBS_SOURCE_TYPE_FILTER;
	info.output_flags = OBS_SOURCE_DO_NOT_DUPLICATE;
	info.get_name = [](void *) -> const char * {
		return "USB Camera PTZ Control";
	};
	info.create = [](obs_data_t *settings, obs_source_t *source) -> void * {
		return ptz_filter_create([&]() -> PTZDevice * { return new PTZUSBCam(settings, source); });
	};
	info.destroy = ptz_filter_destroy;
	info.get_defaults = [](obs_data_t *settings) {
		PTZUSBCam::defaults(settings);
	};
	info.get_properties = ptz_filter_get_properties;
	info.update = ptz_filter_update;
	info.save = ptz_filter_save;
	info.filter_remove = ptz_filter_remove;
	info.icon_type = OBS_ICON_TYPE_CAMERA;
	info.filter_add = ptz_filter_add;
	obs_register_source(&info);
}
