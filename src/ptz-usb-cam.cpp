/* Pan Tilt Zoom USB UVC implementation
	*
	* Copyright 2025 Fabio Ferrari <fabio.ferrar@gmail.com>
	*
	* SPDX-License-Identifier: GPLv2
	*/

#include "imported/qt-wrappers.hpp"
#include "ptz-device.hpp"
#include <cstddef>
#include <obs-data.h>
#include <obs-properties.h>
#include <obs.h>
#include <obs.hpp>
#include "ptz-usb-cam.hpp"

#ifdef __linux__
#include <linux/v4l2-controls.h>
#include <linux/videodev2.h>
#include <sys/ioctl.h>
#include <fcntl.h>
#include <unistd.h>

const std::array<unsigned int, 5> axis_ids = {
    V4L2_CID_PAN_ABSOLUTE, V4L2_CID_TILT_ABSOLUTE,
    V4L2_CID_ZOOM_ABSOLUTE, V4L2_CID_FOCUS_ABSOLUTE,
    V4L2_CID_FOCUS_AUTO
};

class V4L2Control : public PTZControl {
private:
    int fd;
    int query_ctrl(unsigned int i, long *pmin, long *pmax) {
        if (fd == -1) return false;
        struct v4l2_queryctrl queryctrl;
        memset(&queryctrl, 0, sizeof(queryctrl));
        queryctrl.id = axis_ids[i];
        if (ioctl(fd, VIDIOC_QUERYCTRL, &queryctrl) == -1) {
            blog(LOG_ERROR, "VIDIOC_QUERYCTRL failed for axis %d", i);
            return -1;
        }
        *pmin = queryctrl.minimum;
        *pmax = queryctrl.maximum;
        return 0;
    }
    int set_ctrl(unsigned int i, long value) {
        if (fd == -1) return false;
        struct v4l2_control control;
        memset(&control, 0, sizeof(control));
        control.id = axis_ids[i];
        control.value = value;
        if (ioctl(fd, VIDIOC_S_CTRL, &control) == -1) {
            blog(LOG_ERROR, "Failed to set PTZ %d value", i);
            return false;
        }
        return true;
    }

public:
    V4L2Control(const std::string& device) {
        device_path = device;
        fd = open(device_path.c_str(), O_RDWR);
        if (fd == -1) {
            blog(LOG_ERROR, "Failed to open V4L2 device: %s", device_path.c_str());
            return;
        }
        query_ctrl(PTZ_PAN, &min[PTZ_PAN], &max[PTZ_PAN]);
        query_ctrl(PTZ_TILT, &min[PTZ_TILT], &max[PTZ_TILT]);
        query_ctrl(PTZ_ZOOM, &min[PTZ_ZOOM], &max[PTZ_ZOOM]);
        query_ctrl(PTZ_FOCUS, &min[PTZ_FOCUS], &max[PTZ_FOCUS]);
    }
    bool internal_pan(long value) override {
        return set_ctrl(PTZ_PAN, value);
    }
    bool internal_tilt(long value) override {
        return set_ctrl(PTZ_TILT, value);
    }
    bool internal_zoom(long value) override {
        return set_ctrl(PTZ_ZOOM, value);
    }
    bool internal_focus(long value) override {
        return set_ctrl(PTZ_FOCUS, value);
    }
    bool setAutoFocus(bool enabled) override {
        if (fd == -1) return false;
        return set_ctrl(PTZ_FOCUS_AUTO, enabled ? 1 : 0);
    }
    ~V4L2Control() override {
        if (fd == -1) return;
        close(fd);
    }
    bool isValid() const override {
        return fd != -1;
    }
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
    DirectShowControl(const std::string& device) {
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
        hr = CoCreateInstance(CLSID_SystemDeviceEnum, nullptr, CLSCTX_INPROC_SERVER,
                             IID_ICreateDevEnum, (void **)&dev_enum);
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
                        hr = filter_->QueryInterface(IID_IAMCameraControl, (void **)&cam_control_);
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

            if (cam_control_) break;
        }

        enum_moniker->Release();
        dev_enum->Release();

        if (cam_control_ == nullptr) {
            return;
        }
        // blog(LOG_INFO, "Obtained DirectShow filter for device: %s", device_name.c_str());
        long step, default_value, flags;
        hr = cam_control_->GetRange(CameraControl_Pan, &min[PTZ_PAN], &max[PTZ_PAN],
                                            &step, &default_value, &flags);
        if (!FAILED(hr)) {
            hr = cam_control_->GetRange(CameraControl_Tilt, &min[PTZ_TILT], &max[PTZ_TILT],
                                        &step, &default_value, &flags);
        }
        if (!FAILED(hr)) {
            hr = cam_control_->GetRange(CameraControl_Zoom, &min[PTZ_ZOOM], &max[PTZ_ZOOM],
                                        &step, &default_value, &flags);
        }
        if (!FAILED(hr)) {
            hr = cam_control_->GetRange(CameraControl_Focus, &min[PTZ_FOCUS], &max[PTZ_FOCUS],
                                        &step, &default_value, &flags);
        }
        if (FAILED(hr)) {
            blog(LOG_ERROR, "Failed to get ranges: %ld", hr);
            return;
        }
    }
    bool internal_pan(long value) override {
        if (!cam_control_) return false;
        HRESULT hr = cam_control_->Set(CameraControl_Pan, value, CameraControl_Flags_Manual);
        if (FAILED(hr)) {
            blog(LOG_ERROR, "Failed to set Pan: %ld (mapped value: %ld)", hr, value);
            return false;
        }
        return true;
    }
    bool internal_tilt(long value) override {
        if (!cam_control_) return false;
        HRESULT hr = cam_control_->Set(CameraControl_Tilt, value, CameraControl_Flags_Manual);
        if (FAILED(hr)) {
            blog(LOG_ERROR, "Failed to set Tilt: %ld (mapped value: %ld)", hr, value);
            return false;
        }
        return true;
    }
    bool internal_zoom(long value) override {
        if (!cam_control_) return false;
        HRESULT hr = cam_control_->Set(CameraControl_Zoom, value, CameraControl_Flags_Manual);
        if (FAILED(hr)) {
            blog(LOG_ERROR, "Failed to set Zoom: %ld (mapped value: %ld)", hr, value);
            return false;
        }
        return true;
    }
    bool internal_focus(long value) override {
        if (!cam_control_) return false;
        last_focus = value;
        HRESULT hr = cam_control_->Set(CameraControl_Focus, value, CameraControl_Flags_Manual);
        if (FAILED(hr)) {
            blog(LOG_ERROR, "Failed to set Focus: %ld (mapped value: %ld)", hr, value);
            return false;
        }
        return true;
    }
    bool setAutoFocus(bool enabled) override {
        if (!cam_control_) return false;
        HRESULT hr;
        if(!enabled) {
            hr = cam_control_->Set(CameraControl_Focus, last_focus, CameraControl_Flags_Manual);
        } else {
            hr = cam_control_->Set(CameraControl_Focus, 0, CameraControl_Flags_Auto);
        }
        if (FAILED(hr)) {
            blog(LOG_ERROR, "Failed to set AutoFocus: %ld", hr);
            return false;
        }
        return true;
    }
    ~DirectShowControl() override {
        if (cam_control_) {
            cam_control_->Release();
        }
        if (filter_) {
            filter_->Release();
        }
    }
    bool isValid() const override {
        return cam_control_ != nullptr;
    }
};
#endif

void PTZUSBCam::ptz_tick_callback(void *param, float seconds) {
    PTZUSBCam *cam = static_cast<PTZUSBCam *>(param);
    cam->ptz_tick(seconds);
}

PTZUSBCam::PTZUSBCam(OBSData config) : PTZDevice(config)
{
	set_config(config);
	pantilt_abs(0, 0);
	zoom_abs(0);
	set_autofocus(true);
	obs_add_tick_callback(ptz_tick_callback, this);
}

PTZUSBCam::~PTZUSBCam()
{
	obs_remove_tick_callback(ptz_tick_callback, this);
}

QString PTZUSBCam::description()
{
	return QString("USB CAM (UVC)");
}

void PTZUSBCam::set_config(OBSData config)
{
	PTZDevice::set_config(config);
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

OBSData PTZUSBCam::get_config()
{
	OBSData config = PTZDevice::get_config();
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
	return config;
}

obs_properties_t *PTZUSBCam::get_obs_properties()
{
	obs_properties_t *ptz_props = PTZDevice::get_obs_properties();
	obs_properties_remove_by_name(ptz_props, "interface");
	return ptz_props;
}

void PTZUSBCam::do_update()
{
    status &= ~(STATUS_PANTILT_SPEED_CHANGED |
			    STATUS_ZOOM_SPEED_CHANGED | STATUS_FOCUS_SPEED_CHANGED);
}

void PTZUSBCam::ptz_tick(float seconds) {
    tick_elapsed += seconds;
    if (tick_elapsed < 0.05f) // Atualiza a cada 50 ms
        return;
    if (pan_speed != 0.0 || tilt_speed != 0.0) {
        pantilt_rel(pan_speed*tick_elapsed, tilt_speed*tick_elapsed);
    }
    if (zoom_speed != 0.0) {
        zoom_abs(now_pos.zoom + zoom_speed*tick_elapsed);
    }
    if (focus_speed != 0.0) {
        focus_abs(now_pos.focus + focus_speed*tick_elapsed);
    }
    tick_elapsed = 0.0f; // Reseta após a atualização
}

void PTZUSBCam::initialize_and_check_ptz_control()
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
    if(ptz_control_ != nullptr && ptz_control_->isValid() &&
       ptz_control_->getDevicePath() == video_device_id) {
        return;
    }
    blog(LOG_INFO, "Switching PTZ USBUVC device from %s to %s",
        ptz_control_ == nullptr ? "null" : ptz_control_->getDevicePath().c_str(),
        video_device_id.empty() ? "null" : video_device_id.c_str());
    if(ptz_control_ != nullptr) {
        delete ptz_control_;
        ptz_control_ = nullptr;
    }
    if(video_device_id.empty()) {
        return;
    }
#ifdef __linux__
    ptz_control_ = new V4L2Control(video_device_id);
#endif
#ifdef _WIN32
    ptz_control_ = new DirectShowControl(video_device_id);
#endif
}

void PTZUSBCam::pantilt_abs(double pan, double tilt)
{
    initialize_and_check_ptz_control();
    now_pos.pan = std::clamp(pan, -1.0, 1.0);
    now_pos.tilt = std::clamp(tilt, -1.0, 1.0);

    if(ptz_control_ != nullptr && ptz_control_->isValid()) {
        ptz_control_->pan(now_pos.pan);
        ptz_control_->tilt(now_pos.tilt);
    }
}

void PTZUSBCam::pantilt_rel(double pan, double tilt)
{
    pantilt_abs(now_pos.pan+pan, now_pos.tilt+tilt);
}

void PTZUSBCam::pantilt_home()
{
    pantilt_abs(0, 0);
}

void PTZUSBCam::zoom_abs(double pos)
{
    initialize_and_check_ptz_control();
    now_pos.zoom = std::clamp(pos, 0.0, 1.0);
    if(ptz_control_ != nullptr && ptz_control_->isValid()) {
        ptz_control_->zoom(now_pos.zoom);
    }
}

void PTZUSBCam::focus_abs(double pos)
{
    initialize_and_check_ptz_control();
    now_pos.focus = std::clamp(pos, 0.0, 1.0);
    if(ptz_control_ != nullptr && ptz_control_->isValid()) {
        ptz_control_->focus(now_pos.focus);
    }
    return;
}

void PTZUSBCam::set_autofocus(bool enabled)
{
    initialize_and_check_ptz_control();
    now_pos.focusAuto = enabled;
    if(ptz_control_ != nullptr && ptz_control_->isValid()) {
        ptz_control_->setAutoFocus(enabled);
    }
}

void PTZUSBCam::memory_reset(int i)
{
    if (!presets.contains(i))
        return;
    presets.remove(i);
}

void PTZUSBCam::memory_set(int i)
{
    presets[i] = now_pos;
}

void PTZUSBCam::memory_recall(int i)
{
    if (!presets.contains(i))
        return;

    now_pos = presets[i];
    pantilt_abs(now_pos.pan, now_pos.tilt);
    zoom_abs(now_pos.zoom);
    set_autofocus(now_pos.focusAuto);
    if (!now_pos.focusAuto)
        focus_abs(now_pos.focus);
}
