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

#ifdef __linux__
#include <linux/usb/video.h>
#include <linux/uvcvideo.h>
#include <linux/v4l2-controls.h>
#include <linux/videodev2.h>
#include <sys/ioctl.h>
#include <fcntl.h>
#include <unistd.h>

#include <algorithm>
#include <chrono>
#include <memory>
#include <fstream>
#include <iterator>
#include <vector>

/* Logitech UVC extension unit.
 *
 * Selector names and layout are documented in xMRi/PTZControl's
 * ExtensionUnitDefines.h, the only public description of this unit.
 */
namespace {

const uint8_t kLogitechPeripheralGuid[16] = {0x21, 0x2d, 0xe5, 0xff, 0x30, 0x80, 0x2c, 0x4e,
					     0x82, 0xd9, 0xf5, 0x87, 0xd0, 0x05, 0x40, 0xbd};

constexpr uint8_t kSelPanTiltRelative = 0x01;
constexpr uint8_t kSelPanTiltMode = 0x02;

constexpr uint8_t kModeResetBoth = 3;
constexpr uint8_t kModePresetSaveBase = 4;
constexpr uint8_t kModePresetRecallBase = 12;

/* The motor takes 600-900ms to execute one command: 100-200ms of latency
 * before it starts moving, then 400-700ms of travel. Travel time is nearly
 * constant regardless of the magnitude requested, because the firmware varies
 * speed rather than duration.
 *
 * Two consequences drive the design below. A single correctly sized command is
 * both smoother and faster than a sequence of small ones, since the firmware
 * already ramps acceleration. And issuing commands faster than the motor
 * executes them wedges the controller: it keeps returning success from every
 * ioctl and keeps streaming video, while silently ignoring all motion until
 * the camera is power cycled.
 *
 * ptz_tick() asks for movement every 30ms, so requests are accumulated and
 * emitted as one command per window instead of being passed straight through.
 */
constexpr auto kCommandWindow = std::chrono::milliseconds(700);

class LogitechXU {
public:
	explicit LogitechXU(int fd, const std::string &device_path) : fd_(fd)
	{
		unit_ = find_unit_id(device_path);
		if (unit_ == 0)
			return;
		if (!query_ranges())
			unit_ = 0;
	}

	bool isValid() const { return unit_ != 0; }

	/* Accumulate a normalised request and emit at most one command per
	 * window. Returns true if the request was accepted, whether or not it
	 * resulted in an immediate command.
	 */
	bool queueRelative(double pan, double tilt)
	{
		const auto now = std::chrono::steady_clock::now();
		/* Drop anything left over from a previous gesture. Without this
		 * a fraction of a window's worth of movement could sit pending
		 * indefinitely and then be applied to an unrelated request.
		 */
		if (now - last_request_ > kCommandWindow)
			pending_pan_ = pending_tilt_ = 0.0;
		last_request_ = now;
		pending_pan_ += pan;
		pending_tilt_ += tilt;
		return flush();
	}

	bool home()
	{
		pending_pan_ = pending_tilt_ = 0.0;
		uint8_t v = kModeResetBoth;
		return command(kSelPanTiltMode, UVC_SET_CUR, &v, sizeof(v));
	}

	bool presetSave(int slot) { return preset(kModePresetSaveBase, slot); }
	bool presetRecall(int slot) { return preset(kModePresetRecallBase, slot); }

private:
	bool flush()
	{
		const auto now = std::chrono::steady_clock::now();
		if (now - last_command_ < kCommandWindow)
			return true;
		if (pending_pan_ == 0.0 && pending_tilt_ == 0.0)
			return true;

		/* Sustaining full speed for one window travels the full range. */
		const double window = std::chrono::duration<double>(kCommandWindow).count();
		int16_t payload[2];
		payload[0] = scale(pending_pan_ / window, pan_min_, pan_max_);
		payload[1] = scale(pending_tilt_ / window, tilt_min_, tilt_max_);
		pending_pan_ = pending_tilt_ = 0.0;
		last_command_ = now;
		return command(kSelPanTiltRelative, UVC_SET_CUR, payload, sizeof(payload));
	}

	static int16_t scale(double normalised, int lo, int hi)
	{
		const double range = normalised >= 0 ? hi : -lo;
		return static_cast<int16_t>(std::clamp(normalised * range, double(lo), double(hi)));
	}

	bool preset(uint8_t base, int slot)
	{
		if (slot < 1 || slot > 8)
			return false;
		uint8_t v = static_cast<uint8_t>(base + slot - 1);
		return command(kSelPanTiltMode, UVC_SET_CUR, &v, sizeof(v));
	}

	bool command(uint8_t selector, uint8_t request, void *data, uint16_t size)
	{
		struct uvc_xu_control_query q = {};
		q.unit = unit_;
		q.selector = selector;
		q.query = request;
		q.size = size;
		q.data = static_cast<uint8_t *>(data);
		if (ioctl(fd_, UVCIOC_CTRL_QUERY, &q) == -1) {
			blog(LOG_ERROR, "Logitech XU selector 0x%02x failed", selector);
			return false;
		}
		return true;
	}

	/* The byte preceding the GUID in the USB descriptors is the unit id.
	 * There is no ioctl that reports it.
	 */
	uint8_t find_unit_id(const std::string &device_path)
	{
		const auto slash = device_path.find_last_of('/');
		const std::string node = slash == std::string::npos ? device_path : device_path.substr(slash + 1);
		std::ifstream f("/sys/class/video4linux/" + node + "/../../../descriptors", std::ios::binary);
		if (!f)
			return 0;
		const std::vector<uint8_t> d((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
		auto it = std::search(d.begin(), d.end(), std::begin(kLogitechPeripheralGuid),
				      std::end(kLogitechPeripheralGuid));
		if (it == d.end() || it == d.begin())
			return 0;
		return *(it - 1);
	}

	/* Ask the device for its limits rather than hardcoding them: they
	 * differ per model, and the resolution is 1 rather than the handful of
	 * fixed step sizes other tools expose.
	 */
	bool query_ranges()
	{
		int16_t lo[2] = {0, 0};
		int16_t hi[2] = {0, 0};
		if (!command(kSelPanTiltRelative, UVC_GET_MIN, lo, sizeof(lo)))
			return false;
		if (!command(kSelPanTiltRelative, UVC_GET_MAX, hi, sizeof(hi)))
			return false;
		pan_min_ = std::min(lo[0], hi[0]);
		pan_max_ = std::max(lo[0], hi[0]);
		tilt_min_ = std::min(lo[1], hi[1]);
		tilt_max_ = std::max(lo[1], hi[1]);
		return true;
	}

	int fd_;
	uint8_t unit_ = 0;
	int pan_min_ = 0, pan_max_ = 0, tilt_min_ = 0, tilt_max_ = 0;
	double pending_pan_ = 0.0, pending_tilt_ = 0.0;
	std::chrono::steady_clock::time_point last_command_{};
	std::chrono::steady_clock::time_point last_request_{};
};

} // namespace

class V4L2Control : public PTZControl {
private:
	int fd;
	std::unique_ptr<LogitechXU> xu_;
	int query_ctrl(unsigned int i, long *pmin, long *pmax)
	{
		if (fd == -1)
			return false;
		struct v4l2_queryctrl queryctrl = {};
		queryctrl.id = i;
		if (ioctl(fd, VIDIOC_QUERYCTRL, &queryctrl) == -1) {
			blog(LOG_ERROR, "VIDIOC_QUERYCTRL failed for axis %d", i);
			return -1;
		}
		*pmin = queryctrl.minimum;
		*pmax = queryctrl.maximum;
		return 0;
	}
	int set_ctrl(unsigned int i, long value)
	{
		if (fd == -1)
			return false;
		struct v4l2_control control = {};
		control.id = i;
		control.value = value;
		if (ioctl(fd, VIDIOC_S_CTRL, &control) == -1) {
			blog(LOG_ERROR, "Failed to set PTZ %d value", i);
			return false;
		}
		return true;
	}
	int get_ctrl(unsigned int i)
	{
		if (fd == -1)
			return 0;
		struct v4l2_control control = {};
		control.id = i;
		if (ioctl(fd, VIDIOC_G_CTRL, &control) == -1) {
			blog(LOG_ERROR, "VIDIOC_G_CTRL failed for axis %d", i);
			return 0;
		}
		return control.value;
	}

public:
	V4L2Control(const std::string &device)
	{
		device_path = device;
		fd = open(device_path.c_str(), O_RDWR);
		if (fd == -1) {
			blog(LOG_ERROR, "Failed to open V4L2 device: %s", device_path.c_str());
			return;
		}
		query_ctrl(V4L2_CID_PAN_ABSOLUTE, &min.pan, &max.pan);
		query_ctrl(V4L2_CID_TILT_ABSOLUTE, &min.tilt, &max.tilt);
		query_ctrl(V4L2_CID_ZOOM_ABSOLUTE, &min.zoom, &max.zoom);
		query_ctrl(V4L2_CID_FOCUS_ABSOLUTE, &min.focus, &max.focus);
		now_pos.pan = static_cast<double>(get_ctrl(V4L2_CID_PAN_ABSOLUTE)) / max.pan;
		now_pos.tilt = static_cast<double>(get_ctrl(V4L2_CID_TILT_ABSOLUTE)) / max.tilt;
		now_pos.zoom = static_cast<double>(get_ctrl(V4L2_CID_ZOOM_ABSOLUTE)) / max.zoom;
		now_pos.focus = static_cast<double>(get_ctrl(V4L2_CID_FOCUS_ABSOLUTE)) / max.focus;
		now_pos.focusAuto = get_ctrl(V4L2_CID_FOCUS_AUTO);

		xu_ = std::make_unique<LogitechXU>(fd, device_path);
		if (xu_->isValid())
			blog(LOG_INFO, "%s: using Logitech extension unit for pan/tilt", device_path.c_str());
		else
			xu_.reset();
	}
	bool internal_pan(long value) override { return set_ctrl(V4L2_CID_PAN_ABSOLUTE, value); }
	bool internal_tilt(long value) override { return set_ctrl(V4L2_CID_TILT_ABSOLUTE, value); }
	bool internal_zoom(long value) override { return set_ctrl(V4L2_CID_ZOOM_ABSOLUTE, value); }
	bool internal_focus(bool auto_focus, long value) override
	{
		if (auto_focus) {
			return set_ctrl(V4L2_CID_FOCUS_AUTO, 1);
		} else {
			if (get_ctrl(V4L2_CID_FOCUS_AUTO) != 0) {
				set_ctrl(V4L2_CID_FOCUS_AUTO, 0);
			}
			return set_ctrl(V4L2_CID_FOCUS_ABSOLUTE, value);
		}
	}
	bool supportsRelative() const override { return xu_ != nullptr; }
	bool moveRelative(double pan, double tilt) override { return xu_ ? xu_->queueRelative(pan, tilt) : false; }
	bool moveHome() override { return xu_ ? xu_->home() : false; }
	bool supportsHardwarePresets() const override { return xu_ != nullptr; }
	bool presetSave(int slot) override { return xu_ ? xu_->presetSave(slot) : false; }
	bool presetRecall(int slot) override { return xu_ ? xu_->presetRecall(slot) : false; }

	~V4L2Control() override
	{
		if (fd == -1)
			return;
		close(fd);
	}
	bool isValid() const override { return fd != -1; }
};
#endif

#ifdef _WIN32
#include <dshow.h>
#pragma comment(lib, "strmiids.lib") // Linka com DirectShow no Windows

class DirectShowControl : public PTZControl {
private:
	IBaseFilter *filter_ = nullptr;
	IAMCameraControl *cam_control_ = nullptr;
	long last_focus = 0;

public:
	DirectShowControl(const std::string &device)
	{
		device_path = device;
		QString decoded_path = QString::fromStdString(device_path);
		int colon_pos = decoded_path.indexOf(':');
		if (colon_pos != -1) {
			decoded_path = decoded_path.mid(colon_pos + 1);
		}
		decoded_path = decoded_path.split("#22").join("#");
		decoded_path = decoded_path.split("#3A").join(":");
		std::string decoded_std_path = decoded_path.toStdString();
		// blog(LOG_INFO, "PTZ-USB-CAM Device: %s", decoded_std_path.c_str());

		HRESULT hr = CoInitialize(nullptr);
		if (FAILED(hr) && hr != RPC_E_CHANGED_MODE) {
			blog(LOG_ERROR, "Failed to initialize COM: %ld", hr);
			return;
		}

		ICreateDevEnum *dev_enum = nullptr;
		hr = CoCreateInstance(CLSID_SystemDeviceEnum, nullptr, CLSCTX_INPROC_SERVER, IID_ICreateDevEnum,
				      (void **)&dev_enum);
		if (FAILED(hr)) {
			blog(LOG_ERROR, "Failed to create device enumerator: %ld", hr);
			return;
		}

		IEnumMoniker *enum_moniker = nullptr;
		hr = dev_enum->CreateClassEnumerator(CLSID_VideoInputDeviceCategory, &enum_moniker, 0);
		if (FAILED(hr) || !enum_moniker) {
			blog(LOG_ERROR, "Failed to enumerate video devices: %ld", hr);
			dev_enum->Release();
			return;
		}

		IMoniker *moniker = nullptr;
		while (enum_moniker->Next(1, &moniker, nullptr) == S_OK) {
			IPropertyBag *prop_bag = nullptr;
			hr = moniker->BindToStorage(0, 0, IID_IPropertyBag, (void **)&prop_bag);
			if (FAILED(hr)) {
				moniker->Release();
				continue;
			}

			VARIANT var_name;
			VariantInit(&var_name);
			hr = prop_bag->Read(L"DevicePath", &var_name, 0);
			if (SUCCEEDED(hr)) {
				std::wstring w_device_path(var_name.bstrVal);
				std::string device_path1(w_device_path.begin(), w_device_path.end());
				if (device_path1 == decoded_std_path) {
					hr = moniker->BindToObject(0, 0, IID_IBaseFilter, (void **)&filter_);
					if (SUCCEEDED(hr)) {
						hr = filter_->QueryInterface(IID_IAMCameraControl,
									     (void **)&cam_control_);
						if (FAILED(hr)) {
							blog(LOG_ERROR, "Failed to get IAMCameraControl: %ld", hr);
							filter_->Release();
							filter_ = nullptr;
						}
					}
				}
				VariantClear(&var_name);
			}

			prop_bag->Release();
			moniker->Release();

			if (cam_control_)
				break;
		}

		enum_moniker->Release();
		dev_enum->Release();

		if (cam_control_ == nullptr) {
			return;
		}
		// blog(LOG_INFO, "Obtained DirectShow filter for device: %s", device_name.c_str());
		long step, default_value, flags;
		hr = cam_control_->GetRange(CameraControl_Pan, &min.pan, &max.pan, &step, &default_value, &flags);
		if (!FAILED(hr)) {
			hr = cam_control_->GetRange(CameraControl_Tilt, &min.tilt, &max.tilt, &step, &default_value,
						    &flags);
		}
		if (!FAILED(hr)) {
			hr = cam_control_->GetRange(CameraControl_Zoom, &min.zoom, &max.zoom, &step, &default_value,
						    &flags);
		}
		if (!FAILED(hr)) {
			hr = cam_control_->GetRange(CameraControl_Focus, &min.focus, &max.focus, &step, &default_value,
						    &flags);
		}
		if (FAILED(hr)) {
			blog(LOG_ERROR, "Failed to get ranges: %ld", hr);
			return;
		}
		long pan, tilt, zoom, focus;
		cam_control_->Get(CameraControl_Pan, &pan, &flags);
		now_pos.pan = static_cast<double>(pan) / max.pan;
		cam_control_->Get(CameraControl_Tilt, &tilt, &flags);
		now_pos.tilt = static_cast<double>(tilt) / max.tilt;
		cam_control_->Get(CameraControl_Zoom, &zoom, &flags);
		now_pos.zoom = static_cast<double>(zoom) / max.zoom;
		cam_control_->Get(CameraControl_Focus, &focus, &flags);
		now_pos.focus = static_cast<double>(focus) / max.focus;
		now_pos.focusAuto = (flags & CameraControl_Flags_Auto) != 0;
	}
	bool internal_pan(long value) override
	{
		if (!cam_control_)
			return false;
		HRESULT hr = cam_control_->Set(CameraControl_Pan, value, CameraControl_Flags_Manual);
		if (FAILED(hr)) {
			blog(LOG_ERROR, "Failed to set Pan: %ld (mapped value: %ld)", hr, value);
			return false;
		}
		return true;
	}
	bool internal_tilt(long value) override
	{
		if (!cam_control_)
			return false;
		HRESULT hr = cam_control_->Set(CameraControl_Tilt, value, CameraControl_Flags_Manual);
		if (FAILED(hr)) {
			blog(LOG_ERROR, "Failed to set Tilt: %ld (mapped value: %ld)", hr, value);
			return false;
		}
		return true;
	}
	bool internal_zoom(long value) override
	{
		if (!cam_control_)
			return false;
		HRESULT hr = cam_control_->Set(CameraControl_Zoom, value, CameraControl_Flags_Manual);
		if (FAILED(hr)) {
			blog(LOG_ERROR, "Failed to set Zoom: %ld (mapped value: %ld)", hr, value);
			return false;
		}
		return true;
	}
	bool internal_focus(bool auto_focus, long focus) override
	{
		if (!cam_control_)
			return false;
		auto focus_flag = auto_focus ? CameraControl_Flags_Auto : CameraControl_Flags_Manual;
		HRESULT hr;
		hr = cam_control_->Set(CameraControl_Focus, focus, focus_flag);
		if (FAILED(hr)) {
			blog(LOG_ERROR, "Failed to set AutoFocus: %ld", hr);
			return false;
		}
		return true;
	}
	~DirectShowControl() override
	{
		if (cam_control_) {
			cam_control_->Release();
		}
		if (filter_) {
			filter_->Release();
		}
	}
	bool isValid() const override { return cam_control_ != nullptr; }
};
#endif

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

PTZControl *PTZUSBCam::get_ptz_control()
{
	std::string video_device_id = "";
	OBSSourceAutoRelease src = obs_get_source_by_name(QT_TO_UTF8(objectName()));
	if (src) {
		OBSDataAutoRelease psettings = obs_source_get_settings(src);
		if (psettings) {
#ifdef _WIN32
			video_device_id = obs_data_get_string(psettings, "video_device_id");
#else
			video_device_id = obs_data_get_string(psettings, "device_id");
#endif
		}
	}

	// already have the device, and it didn't change: nothing to do
	if (ptz_control_ != nullptr && ptz_control_->isValid() && ptz_control_->getDevicePath() == video_device_id) {
		return ptz_control_;
	}
	blog(LOG_INFO, "Switching PTZ USBUVC device from %s to %s",
	     ptz_control_ == nullptr ? "null" : ptz_control_->getDevicePath().c_str(),
	     video_device_id.empty() ? "null" : video_device_id.c_str());
	if (ptz_control_ != nullptr) {
		delete ptz_control_;
		ptz_control_ = nullptr;
	}
	if (video_device_id.empty()) {
		return nullptr;
	}
#ifdef __linux__
	ptz_control_ = new V4L2Control(video_device_id);
#endif
#ifdef _WIN32
	ptz_control_ = new DirectShowControl(video_device_id);
#endif
	if (ptz_control_ == nullptr || !ptz_control_->isValid()) {
		return nullptr;
	}
	return ptz_control_;
}

void PTZUSBCam::ptz_tick(float seconds)
{
	tick_elapsed += seconds;
	if (tick_elapsed < 0.03f)
		return;
	if (pan_speed != 0.0 || tilt_speed != 0.0) {
		pantilt_rel(pan_speed * tick_elapsed, tilt_speed * tick_elapsed);
	}
	if (zoom_speed != 0.0) {
		auto ptzctrl = get_ptz_control();
		if (ptzctrl) {
			zoom_abs(ptzctrl->getZoom() + zoom_speed * tick_elapsed);
		}
	}
	if (focus_speed != 0.0) {
		auto ptzctrl = get_ptz_control();
		if (ptzctrl) {
			focus_abs(ptzctrl->getFocus() + focus_speed * tick_elapsed);
		}
	}
	tick_elapsed = 0.0f;
}

void PTZUSBCam::pantilt_abs(double pan, double tilt)
{
	auto ptzctrl = get_ptz_control();
	if (!ptzctrl)
		return;
	ptzctrl->pan(pan);
	ptzctrl->tilt(tilt);
}

void PTZUSBCam::pantilt_rel(double pan, double tilt)
{
	auto ptzctrl = get_ptz_control();
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
	auto ptzctrl = get_ptz_control();
	if (!ptzctrl)
		return;
	ptzctrl->zoom(pos);
}

void PTZUSBCam::focus_abs(double pos)
{
	auto ptzctrl = get_ptz_control();
	if (!ptzctrl)
		return;
	ptzctrl->focus(pos);
}

void PTZUSBCam::set_autofocus(bool enabled)
{
	auto ptzctrl = get_ptz_control();
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
	auto ptzctrl = get_ptz_control();
	if (!ptzctrl)
		return;
	/* Prefer the camera's own presets where they exist. Storing a position
	 * we cannot read back would save nothing useful, and hardware presets
	 * also survive a reconnect.
	 */
	if (ptzctrl->supportsHardwarePresets() && ptzctrl->presetSave(i + 1))
		return;
	presets[i] = ptzctrl->getPosition();
}

void PTZUSBCam::memory_recall(int i)
{
	auto hwctrl = get_ptz_control();
	if (hwctrl && hwctrl->supportsHardwarePresets() && hwctrl->presetRecall(i + 1))
		return;
	if (!presets.contains(i))
		return;
	auto now_pos = presets[i];
	pantilt_abs(now_pos.pan, now_pos.tilt);
	zoom_abs(now_pos.zoom);
	set_autofocus(now_pos.focusAuto);
	if (!now_pos.focusAuto)
		focus_abs(now_pos.focus);
}
