/* USB UVC camera backends
 *
 * A backend talks to one USB camera's pan/tilt/zoom/focus controls through
 * whatever the platform provides. Each platform implements the two functions
 * below in its own ptz-usb-backend-<platform>.cpp; PTZUSBCam is platform
 * independent.
 *
 * SPDX-License-Identifier: GPLv2
 */
#pragma once

#include <algorithm>
#include <memory>
#include <string>

struct PtzUsbCamLimits {
	long pan = 0;
	long tilt = 0;
	long zoom = 0;
	long focus = 0;
	// long temperature = 0;
};

struct PtzUsbCamPos {
	double pan = 0;
	double tilt = 0;
	double zoom = 0;
	bool focusAuto = true;
	double focus = 0;
	// bool whitebalAuto = true;
	// double temperature = 0;
};

class PTZUsbBackend {
protected:
	std::string device_path;
	PtzUsbCamLimits min, max;
	/* DirectShow/UVC controls may only accept values at a fixed increment. */
	PtzUsbCamLimits step{1, 1, 1, 1};
	PtzUsbCamPos now_pos;

	/* A camera that lacks a control reports no range for it (max == min, usually
	 * both 0), and there is nothing to move. */
	static bool has_range(long minimum, long maximum) { return maximum > minimum; }

	/* Where a raw position sits relative to the range's maximum. */
	static double ratio(long value, long maximum)
	{
		return maximum != 0 ? static_cast<double>(value) / maximum : 0.0;
	}

	/* Increments below 1 mean the camera didn't say; any value is accepted. */
	static long valid_step(long increment) { return increment > 1 ? increment : 1; }

	static long clamp_to_step(long value, long minimum, long maximum, long increment)
	{
		value = std::clamp(value, minimum, maximum);
		if (increment <= 1)
			return value;

		/* DirectShow defines the increment relative to the property's minimum. */
		long offset = value - minimum;
		long rounded = minimum + ((offset + increment / 2) / increment) * increment;
		return std::clamp(rounded, minimum, maximum);
	}

public:
	virtual ~PTZUsbBackend() {}
	virtual bool internal_pan(long value) = 0;
	bool pan(double value)
	{
		if (!has_range(min.pan, max.pan))
			return false;
		now_pos.pan = std::clamp(value, -1.0, 1.0);
		long pan = clamp_to_step(static_cast<long>(now_pos.pan * max.pan), min.pan, max.pan, step.pan);
		return internal_pan(pan);
	}
	double getPan() const { return now_pos.pan; }
	virtual bool internal_tilt(long value) = 0;
	bool tilt(double value)
	{
		if (!has_range(min.tilt, max.tilt))
			return false;
		now_pos.tilt = std::clamp(value, -1.0, 1.0);
		long tilt = clamp_to_step(static_cast<long>(now_pos.tilt * max.tilt), min.tilt, max.tilt, step.tilt);
		return internal_tilt(tilt);
	}
	double getTilt() const { return now_pos.tilt; }
	virtual bool internal_zoom(long value) = 0;
	bool zoom(double value)
	{
		if (!has_range(min.zoom, max.zoom))
			return false;
		now_pos.zoom = std::clamp(value, 0.0, 1.0);
		long zoom = clamp_to_step(static_cast<long>(now_pos.zoom * max.zoom), min.zoom, max.zoom, step.zoom);
		return internal_zoom(zoom);
	}
	double getZoom() const { return now_pos.zoom; }
	virtual bool internal_focus(bool auto_focus, long value) = 0;
	bool focus(double value)
	{
		if (!has_range(min.focus, max.focus))
			return false;
		now_pos.focus = std::clamp(value, 0.0, 1.0);
		long focus =
			clamp_to_step(static_cast<long>(now_pos.focus * max.focus), min.focus, max.focus, step.focus);
		return internal_focus(false, focus);
	}
	double getFocus() const { return now_pos.focus; }
	bool setAutoFocus(bool enabled)
	{
		long focus =
			clamp_to_step(static_cast<long>(now_pos.focus * max.focus), min.focus, max.focus, step.focus);
		return internal_focus(enabled, focus);
	}
	struct PtzUsbCamPos getPosition() const { return now_pos; }
	/* Where the camera can go, as the backend found out from the camera. An axis
	 * the camera doesn't have has no range (see has_range()). */
	PtzUsbCamLimits getMin() const { return min; }
	PtzUsbCamLimits getMax() const { return max; }
	PtzUsbCamLimits getStep() const { return step; }
	/* Ask the camera where it is, rather than reporting where the backend last
	 * told it to go (which is what getPosition() does until this is called).
	 * Cameras answer as they like: some give the position of the motor, some
	 * just the last position they were sent. Returns false if the backend
	 * can't, or the camera is gone. */
	virtual bool refreshPosition() { return false; }
	std::string getDevicePath() { return device_path; }
	virtual bool isValid() const = 0;
	/* Check that the camera is still there, and return whether it is. isValid()
	 * only knows what the backend has found out so far, and several can only
	 * find out that a camera was unplugged by asking the camera or the OS. So
	 * this is for when there is nothing else to send it: a cheap request that
	 * any camera answers. */
	virtual bool checkAlive() { return isValid(); }
};

/* Name of the OBS video capture source setting that identifies the camera. */
const char *ptz_usb_source_setting_key();

/* Open the camera that ptz_usb_source_setting_key() named. The result may be
 * !isValid() (never nullptr). If it fails, log why only when report is set: the
 * caller keeps retrying a camera that isn't there, and doesn't want the same
 * message every time. */
std::unique_ptr<PTZUsbBackend> ptz_usb_backend_create(const std::string &device_id, bool report);
