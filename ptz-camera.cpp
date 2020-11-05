/* Pan Tilt Zoom camera instance
 *
 * Copyright 2020 Grant Likely <grant.likely@secretlab.ca>
 *
 * SPDX-License-Identifier: GPLv2
 */

#include "ptz-camera.hpp"

PTZCamera::PTZCamera(VISCAInterface_t *iface, int address)
	: interface(iface)
{
	camera.address = address;
	init();
}

PTZCamera::~PTZCamera()
{
	pantilt_stop();
	zoom_stop();
}

void PTZCamera::init()
{
	VISCA_clear(interface, &camera);
	VISCA_get_camera_info(interface, &camera);
}

void PTZCamera::pantilt_stop()
{
	VISCA_set_pantilt_stop(interface, &camera, 0, 0);
}

#define ptzcamera_pantilt_wrapper(dir) \
	void PTZCamera::pantilt_##dir(uint32_t pan_speed, uint32_t tilt_speed) \
	{ VISCA_set_pantilt_##dir(interface, &camera, pan_speed, tilt_speed); }
ptzcamera_pantilt_wrapper(up)
ptzcamera_pantilt_wrapper(upleft)
ptzcamera_pantilt_wrapper(upright)
ptzcamera_pantilt_wrapper(left)
ptzcamera_pantilt_wrapper(right)
ptzcamera_pantilt_wrapper(down)
ptzcamera_pantilt_wrapper(downleft)
ptzcamera_pantilt_wrapper(downright)

void PTZCamera::pantilt_home()
{
	VISCA_set_pantilt_home(interface, &camera);
}

#define ptzcamera_zoom_wrapper(dir) \
	void PTZCamera::zoom_##dir() \
	{ VISCA_set_zoom_##dir(interface, &camera); }
ptzcamera_zoom_wrapper(stop)
ptzcamera_zoom_wrapper(tele)
ptzcamera_zoom_wrapper(wide)
