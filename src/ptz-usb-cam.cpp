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
#include "ptz-thumbnail.hpp"

PTZUSBCam::PTZUSBCam(OBSData config, obs_source_t *source)
	: PTZDevice(config, source),
	  worker_(new PTZUsbWorker(ptz_usb_backend_create))
{
	update(config);

	/* The worker's thread reports back through queued signals, which are
	 * delivered here on the device's thread. */
	connect(worker_.get(), &PTZUsbWorker::connectedChanged, this,
		[this](bool connected) { setConnected(connected); });
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

void PTZUSBCam::update(OBSData config)
{
	PTZDevice::update(config);
	migrateCameraPresets(config);
}

/* Versions that kept the presets as the camera's had a "camera:<slot>" preset
 * in the list for each position in "presets_memory". Now they are local presets
 * with the same values, keeping their name, thumbnail and place in the order. A
 * slot with no position has nothing to go to, and is dropped. */
void PTZUSBCam::migrateCameraPresets(obs_data_t *config)
{
	OBSDataArrayAutoRelease legacy = obs_data_get_array(config, "presets_memory");
	bool changed = false;
	for (int i = m_presets.size() - 1; i >= 0; i--) {
		Preset &preset = m_presets[i];
		if (!preset.onCamera())
			continue;
		changed = true;
		QString key = presetKey(preset.id);
		OBSDataAutoRelease position;
		for (size_t n = 0; legacy && n < obs_data_array_count(legacy); n++) {
			OBSDataAutoRelease item = obs_data_array_item(legacy, n);
			if (QString::number(obs_data_get_int(item, "preset_id")) == key) {
				position = item.Get();
				obs_data_addref(position);
				break;
			}
		}
		QString old = preset.id;
		if (!position) {
			m_order.removeAll(old);
			ptz_thumbnail_remove(preset.thumbnail);
			m_presets.removeAt(i);
			continue;
		}
		preset.id = presetId(QStringLiteral("local"), key);
		m_order.replaceInStrings(old, preset.id);
		preset.cameraName.clear();
		preset.values = obs_data_create();
		obs_data_release(preset.values);
		obs_data_set_double(preset.values, "pan", obs_data_get_double(position, "pan"));
		obs_data_set_double(preset.values, "tilt", obs_data_get_double(position, "tilt"));
		obs_data_set_double(preset.values, "zoom", obs_data_get_double(position, "zoom"));
		OBSDataAutoRelease focus = obs_data_create();
		obs_data_set_double(focus, "position", obs_data_get_double(position, "focus"));
		obs_data_set_bool(focus, "af_enabled", obs_data_get_bool(position, "focusauto"));
		obs_data_set_obj(preset.values, "focus", focus);
	}
	if (changed) {
		reconcileOrder();
		persist();
	}
}

/* The camera's positions are the device's local presets now */
void PTZUSBCam::persistState(obs_data_t *config) const
{
	PTZDevice::persistState(config);
	obs_data_erase(config, "presets_memory");
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
	has_pantilt = hasPan || hasTilt;
	has_zoom = hasZoom;
	has_focus = hasFocus;
	featuresChanged();

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

/* What the camera has a control for. Its presets are the device's local ones,
 * of the positions it was last sent to. Nothing until it has said: a webcam
 * often has no pan or tilt. */
PTZDevice::Features PTZUSBCam::features() const
{
	Features features;
	if (has_pantilt)
		features |= PanTilt | PanTiltAbs | PanTiltRel | Home;
	if (has_zoom)
		features |= Zoom | ZoomAbs;
	if (has_focus)
		features |= Focus | FocusAbs | AutoFocus;
	return features;
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

void ptz_usb_cam_register_filter()
{
	struct obs_source_info info = {};
	info.id = "ca.secretlab.obs-ptz.usb-cam";
	info.type = OBS_SOURCE_TYPE_FILTER;
	info.output_flags = OBS_SOURCE_VIDEO | OBS_SOURCE_DO_NOT_DUPLICATE;
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
