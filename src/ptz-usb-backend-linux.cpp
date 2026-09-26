/* Pan Tilt Zoom USB UVC implementation for Linux (V4L2)
	*
	* Copyright 2025 Fabio Ferrari <fabioferrari@gmail.com>
	*
	* SPDX-License-Identifier: GPLv2
	*/

#include <obs.h>
#include "ptz.h"
#include "ptz-usb-backend.hpp"
#include <linux/v4l2-controls.h>
#include <linux/videodev2.h>
#include <cerrno>
#include <sys/ioctl.h>
#include <fcntl.h>
#include <unistd.h>

class V4L2Control : public PTZUsbBackend {
private:
	int fd;
	bool has_focus_auto_ = false;
	/* Fill in the range of a control and return true if the camera has it. Most
	 * webcams lack some of these, which is expected. */
	bool query_ctrl(unsigned int i, long *pmin, long *pmax, long *pstep)
	{
		if (fd == -1)
			return false;
		struct v4l2_queryctrl queryctrl = {};
		queryctrl.id = i;
		if (ioctl(fd, VIDIOC_QUERYCTRL, &queryctrl) == -1 || (queryctrl.flags & V4L2_CTRL_FLAG_DISABLED)) {
			blog(LOG_INFO, "%s has no control 0x%x", device_path.c_str(), i);
			return false;
		}
		*pmin = queryctrl.minimum;
		*pmax = queryctrl.maximum;
		if (pstep)
			*pstep = valid_step(queryctrl.step);
		return true;
	}
	/* Once the camera is unplugged every ioctl on its fd fails with ENODEV. Give
	 * up the fd so isValid() goes false and the driver looks for the camera
	 * again. Takes the errno of the failed ioctl; true if that means it's gone. */
	bool camera_lost(int err)
	{
		if (err != ENODEV)
			return false;
		if (fd != -1) {
			blog(LOG_WARNING, "USB camera %s was unplugged", device_path.c_str());
			close(fd);
			fd = -1;
		}
		return true;
	}
	int set_ctrl(unsigned int i, long value)
	{
		if (fd == -1)
			return false;
		struct v4l2_control control = {};
		control.id = i;
		control.value = value;
		if (ioctl(fd, VIDIOC_S_CTRL, &control) == -1) {
			if (!camera_lost(errno))
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
			if (!camera_lost(errno))
				blog(LOG_ERROR, "VIDIOC_G_CTRL failed for axis %d", i);
			return 0;
		}
		return control.value;
	}

public:
	V4L2Control(const std::string &device, bool report)
	{
		device_path = device;
		fd = open(device_path.c_str(), O_RDWR);
		if (fd == -1) {
			if (report)
				blog(LOG_ERROR, "Failed to open V4L2 device: %s", device_path.c_str());
			return;
		}
		if (query_ctrl(V4L2_CID_PAN_ABSOLUTE, &min.pan, &max.pan, &step.pan))
			now_pos.pan = ratio(get_ctrl(V4L2_CID_PAN_ABSOLUTE), max.pan);
		if (query_ctrl(V4L2_CID_TILT_ABSOLUTE, &min.tilt, &max.tilt, &step.tilt))
			now_pos.tilt = ratio(get_ctrl(V4L2_CID_TILT_ABSOLUTE), max.tilt);
		if (query_ctrl(V4L2_CID_ZOOM_ABSOLUTE, &min.zoom, &max.zoom, &step.zoom))
			now_pos.zoom = ratio(get_ctrl(V4L2_CID_ZOOM_ABSOLUTE), max.zoom);
		if (query_ctrl(V4L2_CID_FOCUS_ABSOLUTE, &min.focus, &max.focus, &step.focus))
			now_pos.focus = ratio(get_ctrl(V4L2_CID_FOCUS_ABSOLUTE), max.focus);
		long auto_min, auto_max;
		has_focus_auto_ = query_ctrl(V4L2_CID_FOCUS_AUTO, &auto_min, &auto_max, nullptr);
		now_pos.focusAuto = has_focus_auto_ && get_ctrl(V4L2_CID_FOCUS_AUTO) != 0;
	}
	bool internal_pan(long value) override { return set_ctrl(V4L2_CID_PAN_ABSOLUTE, value); }
	bool internal_tilt(long value) override { return set_ctrl(V4L2_CID_TILT_ABSOLUTE, value); }
	bool internal_zoom(long value) override { return set_ctrl(V4L2_CID_ZOOM_ABSOLUTE, value); }
	bool internal_focus(bool auto_focus, long value) override
	{
		if (auto_focus)
			return has_focus_auto_ && set_ctrl(V4L2_CID_FOCUS_AUTO, 1);
		if (has_focus_auto_ && get_ctrl(V4L2_CID_FOCUS_AUTO) != 0)
			set_ctrl(V4L2_CID_FOCUS_AUTO, 0);
		return has_range(min.focus, max.focus) && set_ctrl(V4L2_CID_FOCUS_ABSOLUTE, value);
	}
	~V4L2Control() override
	{
		if (fd == -1)
			return;
		close(fd);
	}
	bool isValid() const override { return fd != -1; }
};

const char *ptz_usb_source_setting_key()
{
	return "device_id";
}

std::unique_ptr<PTZUsbBackend> ptz_usb_backend_create(const std::string &device_id, bool report)
{
	return std::make_unique<V4L2Control>(device_id, report);
}
