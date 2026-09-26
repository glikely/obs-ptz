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
#include <sys/ioctl.h>
#include <fcntl.h>
#include <unistd.h>

class V4L2Control : public PTZUsbBackend {
private:
	int fd;
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

PTZUsbBackend *ptz_usb_backend_create(const std::string &device_id)
{
	return new V4L2Control(device_id);
}
