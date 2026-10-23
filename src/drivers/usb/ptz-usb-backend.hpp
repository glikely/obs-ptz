/* USB UVC camera backends
 *
 * A backend talks to one USB camera's pan/tilt/zoom/focus controls through
 * whatever the platform provides. Each platform implements the two functions
 * below in its own ptz-usb-backend-<platform>.cpp; PTZUSBCam is platform
 * independent.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#pragma once

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <memory>
#include <string>

struct PtzUsbCamLimits {
	long pan = 0;
	long tilt = 0;
	long zoom = 0;
	long focus = 0;
	// long temperature = 0;
};

/* The speeds a camera moves pan and tilt at when told to keep moving (relative
 * pan/tilt), slowest and fastest, as the camera counts them */
struct PtzUsbCamSpeeds {
	long pan_min = 0;
	long pan_max = 0;
	long tilt_min = 0;
	long tilt_max = 0;
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
	/* Set by a backend whose camera can start and stop its pan/tilt motors
	 * (see pantiltSpeed()), with the speeds it can do */
	bool has_pantilt_speed = false;
	PtzUsbCamSpeeds speeds;
	/* Set by a backend whose camera can focus by itself */
	bool has_autofocus = false;

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

	/* A signed speed in the camera's units for a speed of -1..1: zero
	 * stops, anything else is at least the camera's slowest */
	static long camera_speed(double value, long slowest, long fastest)
	{
		if (value == 0)
			return 0;
		slowest = std::max(slowest, 1L);
		fastest = std::max(fastest, slowest);
		double magnitude = std::min(std::fabs(value), 1.0);
		long speed = slowest + std::lround(magnitude * (fastest - slowest));
		return value < 0 ? -speed : speed;
	}

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

private:
	bool sent_pantilt_speed_ = false;
	long last_pan_speed_ = 0, last_tilt_speed_ = 0;

public:
	virtual ~PTZUsbBackend() {}

	/* Start, change or stop a relative pan/tilt: signed speeds in the
	 * camera's units (see PtzUsbCamSpeeds), positive pans right and tilts
	 * up, zero stops that axis */
	virtual bool internal_pantilt_speed(long pan, long tilt)
	{
		(void)pan;
		(void)tilt;
		return false;
	}
	/* Whether pan and tilt are driven by speed, with pantiltSpeed(), rather
	 * than moved to a position. Only for a camera that has no position to
	 * go to, such as Logitech's PTZ Pro 2: where a camera has both, moving
	 * to positions does more (presets, home, moves by a distance). Setting
	 * PTZ_USB_PANTILT_SPEED in the environment uses speed on any camera
	 * that has it, to try it out. */
	bool usesPanTiltSpeed() const
	{
		if (!has_pantilt_speed)
			return false;
		static const bool forced = std::getenv("PTZ_USB_PANTILT_SPEED") != nullptr;
		return forced || !(has_range(min.pan, max.pan) || has_range(min.tilt, max.tilt));
	}
	/* Keep pan and tilt moving at speeds of -1..1, until told 0. Only sends
	 * the camera what has changed in its own units, since a camera can be
	 * slow to take a command. */
	bool pantiltSpeed(double pan, double tilt)
	{
		if (!usesPanTiltSpeed())
			return false;
		long p = camera_speed(pan, speeds.pan_min, speeds.pan_max);
		long t = camera_speed(tilt, speeds.tilt_min, speeds.tilt_max);
		if (sent_pantilt_speed_ && p == last_pan_speed_ && t == last_tilt_speed_)
			return true;
		sent_pantilt_speed_ = internal_pantilt_speed(p, t);
		last_pan_speed_ = p;
		last_tilt_speed_ = t;
		return sent_pantilt_speed_;
	}
	PtzUsbCamSpeeds getSpeeds() const { return speeds; }

	/* How the camera can be moved, as the backend found out when it opened
	 * it, and which way pan/tilt is driven: for the log, to debug a camera
	 * that doesn't move as it should */
	std::string movementModes() const
	{
		auto range = [](long lo, long hi, long increment) {
			std::string r = std::to_string(lo) + ".." + std::to_string(hi);
			if (increment > 1)
				r += " step " + std::to_string(increment);
			return r;
		};
		bool position = has_range(min.pan, max.pan) || has_range(min.tilt, max.tilt);
		std::string modes;
		auto add = [&](const std::string &mode) {
			modes += (modes.empty() ? "" : ", ") + mode;
		};
		if (position)
			add("pan/tilt to a position (pan " + range(min.pan, max.pan, step.pan) + ", tilt " +
			    range(min.tilt, max.tilt, step.tilt) + ")");
		if (has_pantilt_speed)
			add("pan/tilt at a speed (pan " + range(speeds.pan_min, speeds.pan_max, 1) + ", tilt " +
			    range(speeds.tilt_min, speeds.tilt_max, 1) + ")");
		if (has_range(min.zoom, max.zoom))
			add("zoom to a position (" + range(min.zoom, max.zoom, step.zoom) + ")");
		if (has_range(min.focus, max.focus))
			add("focus to a position (" + range(min.focus, max.focus, step.focus) + ")");
		if (has_autofocus)
			add("autofocus");
		if (modes.empty())
			return "no pan, tilt, zoom or focus";
		if (usesPanTiltSpeed())
			modes += "; pan/tilt is driven at a speed";
		else if (position)
			modes += "; pan/tilt is driven to positions";
		return modes;
	}

	virtual bool internal_pan(long value) = 0;
	bool pan(double value)
	{
		if (!has_range(min.pan, max.pan) || usesPanTiltSpeed())
			return false;
		now_pos.pan = std::clamp(value, -1.0, 1.0);
		long pan = clamp_to_step(static_cast<long>(now_pos.pan * max.pan), min.pan, max.pan, step.pan);
		return internal_pan(pan);
	}
	double getPan() const { return now_pos.pan; }
	virtual bool internal_tilt(long value) = 0;
	bool tilt(double value)
	{
		if (!has_range(min.tilt, max.tilt) || usesPanTiltSpeed())
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
