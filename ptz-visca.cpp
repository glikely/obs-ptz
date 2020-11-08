/* Pan Tilt Zoom visca instance
 *
 * Copyright 2020 Grant Likely <grant.likely@secretlab.ca>
 *
 * SPDX-License-Identifier: GPLv2
 */

#include "ptz-visca.hpp"

PTZVisca::PTZVisca(VISCAInterface_t *iface, int address)
	: PTZDevice(), interface(iface)
{
	camera.address = address;
	init();
}

PTZVisca::~PTZVisca()
{
	pantilt_stop();
	zoom_stop();
}

void PTZVisca::init()
{
	VISCA_clear(interface, &camera);
	VISCA_get_camera_info(interface, &camera);
}

void PTZVisca::pantilt(double pan, double tilt)
{
	VISCA_set_pantilt(interface, &camera, pan * 10, tilt * 10);
}

void PTZVisca::pantilt_stop()
{
	VISCA_set_pantilt_stop(interface, &camera, 0, 0);
}

#define ptzvisca_pantilt_wrapper(dir) \
	void PTZVisca::pantilt_##dir(uint32_t pan_speed, uint32_t tilt_speed) \
	{ VISCA_set_pantilt_##dir(interface, &camera, pan_speed, tilt_speed); }
ptzvisca_pantilt_wrapper(up)
ptzvisca_pantilt_wrapper(upleft)
ptzvisca_pantilt_wrapper(upright)
ptzvisca_pantilt_wrapper(left)
ptzvisca_pantilt_wrapper(right)
ptzvisca_pantilt_wrapper(down)
ptzvisca_pantilt_wrapper(downleft)
ptzvisca_pantilt_wrapper(downright)

void PTZVisca::pantilt_home()
{
	VISCA_set_pantilt_home(interface, &camera);
}

#define ptzvisca_zoom_wrapper(dir) \
	void PTZVisca::zoom_##dir() \
	{ VISCA_set_zoom_##dir(interface, &camera); }
ptzvisca_zoom_wrapper(stop)
ptzvisca_zoom_wrapper(tele)
ptzvisca_zoom_wrapper(wide)
