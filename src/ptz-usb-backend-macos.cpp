/* Pan Tilt Zoom USB UVC control on macOS, straight over IOKit
 *
 * AVFoundation has no public PTZ API, so this sends UVC class requests to the
 * camera's VideoControl interface as control transfers on the device's default
 * endpoint. That needs the device opened, but not the interface claimed, so it
 * works while OBS's own capture source is streaming from the camera.
 *
 * SPDX-License-Identifier: GPLv2
 */

#include <algorithm>
#include <cstring>
#include <obs.h>
#include <CoreFoundation/CoreFoundation.h>
#include <IOKit/IOCFPlugIn.h>
#include <IOKit/IOKitLib.h>
#include <IOKit/usb/IOUSBLib.h>
#include "ptz.h"
#include "ptz-usb-backend.hpp"
#include "uvc-protocol.hpp"

namespace {

constexpr UInt32 REQUEST_TIMEOUT_MS = 250;

bool registry_int(io_service_t service, CFStringRef key, int64_t *out)
{
	CFTypeRef ref = IORegistryEntryCreateCFProperty(service, key, kCFAllocatorDefault, 0);
	if (!ref)
		return false;
	bool ok = CFGetTypeID(ref) == CFNumberGetTypeID() &&
		  CFNumberGetValue((CFNumberRef)ref, kCFNumberSInt64Type, out);
	CFRelease(ref);
	return ok;
}

/* Find the IOUSBHostDevice that AVFoundation's uniqueID points at */
io_service_t find_usb_device(const uvc::UsbDeviceId &id)
{
	io_iterator_t iter = IO_OBJECT_NULL;
	if (IOServiceGetMatchingServices(kIOMainPortDefault, IOServiceMatching("IOUSBHostDevice"), &iter) !=
	    KERN_SUCCESS)
		return IO_OBJECT_NULL;

	io_service_t found = IO_OBJECT_NULL;
	while (io_service_t service = IOIteratorNext(iter)) {
		int64_t vendor, product, location;
		if (registry_int(service, CFSTR("idVendor"), &vendor) &&
		    registry_int(service, CFSTR("idProduct"), &product) &&
		    registry_int(service, CFSTR("locationID"), &location) && vendor == id.vendor_id &&
		    product == id.product_id && static_cast<uint32_t>(location) == id.location_id) {
			found = service;
			break;
		}
		IOObjectRelease(service);
	}
	IOObjectRelease(iter);
	return found;
}

typedef IOUSBDeviceInterface320 UsbDevice;

class IOKitUVCControl : public PTZUsbBackend {
private:
	UsbDevice **dev_ = nullptr;
	bool opened_ = false;
	bool valid_ = false;
	bool report_;
	uvc::CameraTerminal terminal_{0, 0};

	bool has_pantilt_ = false;
	bool has_zoom_ = false;
	bool has_focus_ = false;
	bool has_focus_auto_ = false;

	/* Pan and tilt are written together, so remember both axes */
	int32_t pan_ = 0;
	int32_t tilt_ = 0;
	bool sent_pantilt_ = false;
	uint8_t last_pantilt_[uvc::PANTILT_LEN] = {};

	bool open_device(const uvc::UsbDeviceId &id)
	{
		io_service_t service = find_usb_device(id);
		if (service == IO_OBJECT_NULL) {
			if (report_)
				blog(LOG_ERROR, "USB camera %s is not attached", device_path.c_str());
			return false;
		}
		IOCFPlugInInterface **plugin = nullptr;
		SInt32 score = 0;
		IOReturn kr = IOCreatePlugInInterfaceForService(service, kIOUSBDeviceUserClientTypeID,
								kIOCFPlugInInterfaceID, &plugin, &score);
		IOObjectRelease(service);
		if (kr != kIOReturnSuccess || !plugin) {
			if (report_)
				blog(LOG_ERROR, "Failed to create USB plugin interface for %s: 0x%x",
				     device_path.c_str(), kr);
			return false;
		}
		HRESULT hr = (*plugin)->QueryInterface(plugin, CFUUIDGetUUIDBytes(kIOUSBDeviceInterfaceID320),
						       (LPVOID *)&dev_);
		(*plugin)->Release(plugin);
		if (hr != S_OK || !dev_) {
			dev_ = nullptr;
			if (report_)
				blog(LOG_ERROR, "Failed to get USB device interface for %s", device_path.c_str());
			return false;
		}
		kr = (*dev_)->USBDeviceOpen(dev_);
		if (kr != kIOReturnSuccess) {
			if (report_)
				blog(LOG_ERROR, "Failed to open USB camera %s: 0x%x", device_path.c_str(), kr);
			return false;
		}
		opened_ = true;
		return true;
	}

	/* The Camera Terminal is in the active configuration's descriptor */
	bool find_terminal()
	{
		UInt8 active = 0, count = 0;
		if ((*dev_)->GetConfiguration(dev_, &active) != kIOReturnSuccess ||
		    (*dev_)->GetNumberOfConfigurations(dev_, &count) != kIOReturnSuccess)
			return false;
		for (UInt8 i = 0; i < count; i++) {
			IOUSBConfigurationDescriptorPtr cfg = nullptr;
			if ((*dev_)->GetConfigurationDescriptorPtr(dev_, i, &cfg) != kIOReturnSuccess || !cfg)
				continue;
			if (cfg->bConfigurationValue != active)
				continue;
			auto terminal = uvc::find_camera_terminal(reinterpret_cast<const uint8_t *>(cfg),
								  OSSwapLittleToHostInt16(cfg->wTotalLength));
			if (!terminal)
				return false;
			terminal_ = *terminal;
			return true;
		}
		return false;
	}

	/* Send a UVC class request to the VideoControl interface. wValue and wIndex
	 * are as the UVC spec has them. If the camera is gone that is noticed here,
	 * and the backend stops being valid. */
	bool transfer(bool in, uint8_t request, uint16_t value, uint16_t index, uint8_t *data, uint16_t length,
		      bool quiet)
	{
		if (!valid_)
			return false;
		IOUSBDevRequestTO req = {};
		req.bmRequestType = USBmakebmRequestType(in ? kUSBIn : kUSBOut, kUSBClass, kUSBInterface);
		req.bRequest = request;
		req.wValue = value;
		req.wIndex = index;
		req.wLength = length;
		req.pData = data;
		req.noDataTimeout = REQUEST_TIMEOUT_MS;
		req.completionTimeout = REQUEST_TIMEOUT_MS;
		IOReturn kr = (*dev_)->DeviceRequestTO(dev_, &req);
		if (kr == kIOReturnSuccess)
			return true;
		if (kr == kIOReturnNoDevice || kr == kIOReturnNotAttached) {
			blog(LOG_WARNING, "USB camera %s was unplugged", device_path.c_str());
			valid_ = false;
		} else if (!quiet) {
			blog(LOG_ERROR, "UVC request 0x%02x value 0x%04x failed for %s: 0x%x", request, value,
			     device_path.c_str(), kr);
		}
		return false;
	}

	/* A request to a control of the Camera Terminal. Not every camera implements
	 * every control; unsupported ones stall, which is expected while probing
	 * (quiet), but a failure to set is not. */
	bool request(bool in, uint8_t request, uint8_t selector, uint8_t *data, uint16_t length, bool quiet = false)
	{
		return transfer(in, request, uvc::control_value(selector),
				uvc::control_index(terminal_.terminal_id, terminal_.interface_number), data, length,
				quiet);
	}

	/* Fill in min, max and res for a control, returning whether it responded */
	bool get_range(uint8_t selector, uint16_t length, uint8_t *lo, uint8_t *hi, uint8_t *res)
	{
		return request(true, uvc::GET_MIN, selector, lo, length, true) &&
		       request(true, uvc::GET_MAX, selector, hi, length, true) &&
		       request(true, uvc::GET_RES, selector, res, length, true);
	}

	void probe_pantilt()
	{
		uint8_t lo[uvc::PANTILT_LEN], hi[uvc::PANTILT_LEN], res[uvc::PANTILT_LEN], cur[uvc::PANTILT_LEN];
		if (!get_range(uvc::CT_PANTILT_ABSOLUTE, uvc::PANTILT_LEN, lo, hi, res))
			return;
		min.pan = uvc::get_le32(lo);
		min.tilt = uvc::get_le32(lo + 4);
		max.pan = uvc::get_le32(hi);
		max.tilt = uvc::get_le32(hi + 4);
		step.pan = valid_step(uvc::get_le32(res));
		step.tilt = valid_step(uvc::get_le32(res + 4));
		if (max.pan <= min.pan && max.tilt <= min.tilt)
			return;
		has_pantilt_ = true;
		if (request(true, uvc::GET_CUR, uvc::CT_PANTILT_ABSOLUTE, cur, uvc::PANTILT_LEN, true)) {
			pan_ = uvc::get_le32(cur);
			tilt_ = uvc::get_le32(cur + 4);
			now_pos.pan = std::clamp(ratio(pan_, max.pan), -1.0, 1.0);
			now_pos.tilt = std::clamp(ratio(tilt_, max.tilt), -1.0, 1.0);
		}
	}

	bool probe_u16(uint8_t selector, long *lo, long *hi, long *res, double *now)
	{
		uint8_t l[uvc::ZOOM_LEN], h[uvc::ZOOM_LEN], r[uvc::ZOOM_LEN], c[uvc::ZOOM_LEN];
		if (!get_range(selector, 2, l, h, r))
			return false;
		*lo = uvc::get_le16(l);
		*hi = uvc::get_le16(h);
		*res = valid_step(uvc::get_le16(r));
		if (*hi <= *lo)
			return false;
		if (request(true, uvc::GET_CUR, selector, c, 2, true))
			*now = std::clamp(ratio(uvc::get_le16(c), *hi), 0.0, 1.0);
		return true;
	}

	bool set_u16(uint8_t selector, long value)
	{
		uint8_t data[2];
		uvc::put_le16(data, static_cast<uint16_t>(std::clamp(value, 0L, 0xffffL)));
		return request(false, uvc::SET_CUR, selector, data, 2);
	}

	bool set_focus_auto(bool enabled)
	{
		uint8_t data = enabled ? 1 : 0;
		return request(false, uvc::SET_CUR, uvc::CT_FOCUS_AUTO, &data, uvc::FOCUS_AUTO_LEN);
	}

	bool send_pantilt()
	{
		uint8_t data[uvc::PANTILT_LEN];
		uvc::put_le32(data, pan_);
		uvc::put_le32(data + 4, tilt_);
		/* pantilt_abs() sets both axes back to back; the second call finds
		 * the camera already there */
		if (sent_pantilt_ && memcmp(data, last_pantilt_, sizeof(data)) == 0)
			return true;
		if (!request(false, uvc::SET_CUR, uvc::CT_PANTILT_ABSOLUTE, data, uvc::PANTILT_LEN))
			return false;
		memcpy(last_pantilt_, data, sizeof(data));
		sent_pantilt_ = true;
		return true;
	}

public:
	IOKitUVCControl(const std::string &unique_id, bool report) : report_(report)
	{
		device_path = unique_id;
		auto id = uvc::parse_avfoundation_unique_id(unique_id);
		if (!id) {
			if (report_)
				blog(LOG_INFO, "Camera %s is not a USB camera, PTZ is not available",
				     unique_id.c_str());
			return;
		}
		if (!open_device(*id))
			return;
		if (!find_terminal()) {
			if (report_)
				blog(LOG_ERROR, "USB camera %s has no UVC Camera Terminal", device_path.c_str());
			return;
		}
		valid_ = true;

		probe_pantilt();
		has_zoom_ = probe_u16(uvc::CT_ZOOM_ABSOLUTE, &min.zoom, &max.zoom, &step.zoom, &now_pos.zoom);
		has_focus_ = probe_u16(uvc::CT_FOCUS_ABSOLUTE, &min.focus, &max.focus, &step.focus, &now_pos.focus);
		uint8_t autofocus = 0;
		has_focus_auto_ =
			request(true, uvc::GET_CUR, uvc::CT_FOCUS_AUTO, &autofocus, uvc::FOCUS_AUTO_LEN, true);
		now_pos.focusAuto = has_focus_auto_ ? autofocus != 0 : false;

		blog(LOG_INFO,
		     "UVC PTZ ranges: pan=%ld..%ld step=%ld, tilt=%ld..%ld step=%ld, zoom=%ld..%ld step=%ld, "
		     "focus=%ld..%ld step=%ld (%s%s%s%s)",
		     min.pan, max.pan, step.pan, min.tilt, max.tilt, step.tilt, min.zoom, max.zoom, step.zoom,
		     min.focus, max.focus, step.focus, has_pantilt_ ? "pan/tilt " : "", has_zoom_ ? "zoom " : "",
		     has_focus_ ? "focus " : "", has_focus_auto_ ? "autofocus" : "");
	}

	~IOKitUVCControl() override
	{
		if (!dev_)
			return;
		if (opened_)
			(*dev_)->USBDeviceClose(dev_);
		(*dev_)->Release(dev_);
	}

	bool internal_pan(long value) override
	{
		if (!has_pantilt_)
			return false;
		pan_ = static_cast<int32_t>(value);
		return send_pantilt();
	}
	bool internal_tilt(long value) override
	{
		if (!has_pantilt_)
			return false;
		tilt_ = static_cast<int32_t>(value);
		return send_pantilt();
	}
	bool internal_zoom(long value) override { return has_zoom_ && set_u16(uvc::CT_ZOOM_ABSOLUTE, value); }
	bool internal_focus(bool auto_focus, long value) override
	{
		if (auto_focus)
			return has_focus_auto_ && set_focus_auto(true);
		if (has_focus_auto_)
			set_focus_auto(false);
		return has_focus_ && set_u16(uvc::CT_FOCUS_ABSOLUTE, value);
	}
	bool isValid() const override { return valid_; }
	bool checkAlive() override
	{
		/* Ask for the interface's error code, which every UVC camera has. It
		 * doesn't matter whether it answers or stalls: only whether it is there
		 * to do either. */
		uint8_t error_code = 0;
		transfer(true, uvc::GET_CUR, uvc::control_value(uvc::VC_REQUEST_ERROR_CODE_CONTROL),
			 terminal_.interface_number, &error_code, 1, true);
		return valid_;
	}
};

} // namespace

const char *ptz_usb_source_setting_key()
{
	/* av_capture_input stores AVCaptureDevice.uniqueID here */
	return "device";
}

std::unique_ptr<PTZUsbBackend> ptz_usb_backend_create(const std::string &device_id, bool report)
{
	return std::make_unique<IOKitUVCControl>(device_id, report);
}
