/* Pan Tilt Zoom USB UVC implementation for Windows (DirectShow)
	*
	* Copyright 2025 Fabio Ferrari <fabioferrari@gmail.com>
	*
	* SPDX-License-Identifier: GPLv2
	*/

#include <chrono>
#include <QString>
#include <QStringList>
#include <obs.h>
#include "ptz.h"
#include "ptz-usb-backend.hpp"
#include <windows.h>
#include <dshow.h>
#pragma comment(lib, "strmiids.lib") // Linka com DirectShow no Windows

class DirectShowControl : public PTZUsbBackend {
private:
	IBaseFilter *filter_ = nullptr;
	IAMCameraControl *cam_control_ = nullptr;
	long last_focus = 0;

	std::string decoded_path_;
	std::chrono::steady_clock::time_point last_presence_check_;

	/* Find the video input device with this DevicePath. If filter is non-null
	 * the device is bound and returned there (the caller releases it). COM must
	 * already be initialized on the calling thread. */
	static bool find_device(const std::string &path, IBaseFilter **filter, bool report)
	{
		ICreateDevEnum *dev_enum = nullptr;
		HRESULT hr = CoCreateInstance(CLSID_SystemDeviceEnum, nullptr, CLSCTX_INPROC_SERVER, IID_ICreateDevEnum,
					      (void **)&dev_enum);
		if (FAILED(hr)) {
			if (report)
				blog(LOG_ERROR, "Failed to create device enumerator: %ld", hr);
			return false;
		}

		IEnumMoniker *enum_moniker = nullptr;
		hr = dev_enum->CreateClassEnumerator(CLSID_VideoInputDeviceCategory, &enum_moniker, 0);
		dev_enum->Release();
		if (FAILED(hr)) {
			if (report)
				blog(LOG_ERROR, "Failed to enumerate video devices: %ld", hr);
			return false;
		}
		if (!enum_moniker) /* S_FALSE: there are no video devices at all */
			return false;

		bool found = false;
		IMoniker *moniker = nullptr;
		while (!found && enum_moniker->Next(1, &moniker, nullptr) == S_OK) {
			IPropertyBag *prop_bag = nullptr;
			if (SUCCEEDED(moniker->BindToStorage(0, 0, IID_IPropertyBag, (void **)&prop_bag))) {
				VARIANT var_name;
				VariantInit(&var_name);
				if (SUCCEEDED(prop_bag->Read(L"DevicePath", &var_name, 0)) && var_name.bstrVal) {
					std::wstring w_device_path(var_name.bstrVal);
					std::string device_path1(w_device_path.begin(), w_device_path.end());
					if (device_path1 == path) {
						found = !filter || SUCCEEDED(moniker->BindToObject(
									   0, 0, IID_IBaseFilter, (void **)filter));
					}
				}
				VariantClear(&var_name);
				prop_bag->Release();
			}
			moniker->Release();
		}
		enum_moniker->Release();
		return found;
	}

	void release()
	{
		if (cam_control_) {
			cam_control_->Release();
			cam_control_ = nullptr;
		}
		if (filter_) {
			filter_->Release();
			filter_ = nullptr;
		}
	}

	/* Whether the camera is still attached. Any thread can be asked, and it may
	 * not have COM set up, so do it in a scope of its own. If we can't tell,
	 * assume it is. */
	bool still_attached()
	{
		HRESULT hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
		if (FAILED(hr) && hr != RPC_E_CHANGED_MODE)
			return true;
		bool attached = find_device(decoded_path_, nullptr, false);
		if (SUCCEEDED(hr))
			CoUninitialize();
		return attached;
	}

	/* Errors that only mean the camera (or our handle to it) is gone */
	static bool means_camera_gone(HRESULT hr)
	{
		return hr == HRESULT_FROM_WIN32(ERROR_DEVICE_NOT_CONNECTED) ||
		       hr == HRESULT_FROM_WIN32(ERROR_DEV_NOT_EXIST) ||
		       hr == HRESULT_FROM_WIN32(ERROR_NO_SUCH_DEVICE) ||
		       hr == HRESULT_FROM_WIN32(ERROR_DEVICE_REMOVED) || hr == HRESULT_FROM_WIN32(ERROR_INVALID_HANDLE);
	}

	/* A request failed. If that is because the camera was unplugged, drop our
	 * hold on it, so isValid() goes false and the driver looks for it again,
	 * and return true. A camera can also just refuse a value, so for other
	 * errors look for the camera, but not more than once a second: that
	 * enumerates every video device. */
	bool camera_lost(HRESULT hr)
	{
		if (!cam_control_)
			return true;
		if (!means_camera_gone(hr)) {
			auto now = std::chrono::steady_clock::now();
			if (now - last_presence_check_ < std::chrono::seconds(1))
				return false;
			last_presence_check_ = now;
			if (still_attached())
				return false;
		}
		blog(LOG_WARNING, "USB camera %s was unplugged", device_path.c_str());
		release();
		return true;
	}

	/* Read one property's range and position. Cameras rarely have all four, so
	 * a property that is missing (or can't move) is left at 0..0 with a step
	 * of 1, and the base class then won't try to move it. Returns whether the
	 * property has a usable range. */
	bool probe(long property, long *pmin, long *pmax, long *pstep, double *pnow, bool *pauto = nullptr)
	{
		long lo, hi, increment, default_value, flags;
		if (FAILED(cam_control_->GetRange(property, &lo, &hi, &increment, &default_value, &flags)))
			return false;
		long value;
		if (SUCCEEDED(cam_control_->Get(property, &value, &flags))) {
			if (pauto)
				*pauto = (flags & CameraControl_Flags_Auto) != 0;
			if (has_range(lo, hi))
				*pnow = ratio(value, hi);
		}
		if (!has_range(lo, hi))
			return false;
		*pmin = lo;
		*pmax = hi;
		*pstep = valid_step(increment);
		return true;
	}

public:
	DirectShowControl(const std::string &device, bool report)
	{
		device_path = device;
		QString decoded_path = QString::fromStdString(device_path);
		int colon_pos = decoded_path.indexOf(':');
		if (colon_pos != -1) {
			decoded_path = decoded_path.mid(colon_pos + 1);
		}
		decoded_path = decoded_path.split("#22").join("#");
		decoded_path = decoded_path.split("#3A").join(":");
		decoded_path_ = decoded_path.toStdString();
		// blog(LOG_INFO, "PTZ-USB-CAM Device: %s", decoded_path_.c_str());

		HRESULT hr = CoInitialize(nullptr);
		if (FAILED(hr) && hr != RPC_E_CHANGED_MODE) {
			if (report)
				blog(LOG_ERROR, "Failed to initialize COM: %ld", hr);
			return;
		}

		if (!find_device(decoded_path_, &filter_, report)) {
			if (report)
				blog(LOG_WARNING, "USB camera %s not found, or it has no camera controls",
				     device_path.c_str());
			return;
		}
		hr = filter_->QueryInterface(IID_IAMCameraControl, (void **)&cam_control_);
		if (FAILED(hr)) {
			if (report)
				blog(LOG_ERROR, "Failed to get IAMCameraControl: %ld", hr);
			release();
			return;
		}
		// blog(LOG_INFO, "Obtained DirectShow filter for device: %s", device_name.c_str());
		probe(CameraControl_Pan, &min.pan, &max.pan, &step.pan, &now_pos.pan);
		probe(CameraControl_Tilt, &min.tilt, &max.tilt, &step.tilt, &now_pos.tilt);
		probe(CameraControl_Zoom, &min.zoom, &max.zoom, &step.zoom, &now_pos.zoom);
		probe(CameraControl_Focus, &min.focus, &max.focus, &step.focus, &now_pos.focus, &now_pos.focusAuto);
		blog(LOG_INFO,
		     "UVC PTZ ranges: pan=%ld..%ld step=%ld, tilt=%ld..%ld step=%ld, zoom=%ld..%ld step=%ld, "
		     "focus=%ld..%ld step=%ld",
		     min.pan, max.pan, step.pan, min.tilt, max.tilt, step.tilt, min.zoom, max.zoom, step.zoom,
		     min.focus, max.focus, step.focus);
	}
	bool internal_pan(long value) override
	{
		if (!cam_control_)
			return false;
		HRESULT hr = cam_control_->Set(CameraControl_Pan, value, CameraControl_Flags_Manual);
		if (FAILED(hr)) {
			if (!camera_lost(hr))
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
			if (!camera_lost(hr))
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
			if (!camera_lost(hr))
				blog(LOG_ERROR, "Failed to set Zoom: %ld (mapped value: %ld)", hr, value);
			return false;
		}
		return true;
	}
	bool internal_focus(bool auto_focus, long focus) override
	{
		if (!cam_control_)
			return false;
		if (!auto_focus && !has_range(min.focus, max.focus))
			return false;
		auto focus_flag = auto_focus ? CameraControl_Flags_Auto : CameraControl_Flags_Manual;
		HRESULT hr;
		hr = cam_control_->Set(CameraControl_Focus, focus, focus_flag);
		if (FAILED(hr)) {
			if (!camera_lost(hr))
				blog(LOG_ERROR, "Failed to set AutoFocus: %ld", hr);
			return false;
		}
		return true;
	}
	~DirectShowControl() override { release(); }
	bool isValid() const override { return cam_control_ != nullptr; }
};

const char *ptz_usb_source_setting_key()
{
	return "video_device_id";
}

std::unique_ptr<PTZUsbBackend> ptz_usb_backend_create(const std::string &device_id, bool report)
{
	return std::make_unique<DirectShowControl>(device_id, report);
}
