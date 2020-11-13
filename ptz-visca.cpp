/* Pan Tilt Zoom visca instance
 *
 * Copyright 2020 Grant Likely <grant.likely@secretlab.ca>
 *
 * SPDX-License-Identifier: GPLv2
 */

#include "ptz-visca.hpp"

std::map<std::string, VISCAInterface_t*> PTZVisca::interfaces;

PTZVisca::PTZVisca(const char *uart_name, int address)
	: PTZDevice()
{
	int camera_count;
	camera.address = address;
	interface = interfaces[uart_name];
	if (!interface) {
		interface = (VISCAInterface_t*)malloc(sizeof(VISCAInterface_t));
		interfaces[uart_name] = interface;
		if (VISCA_open_serial(interface, uart_name) != VISCA_SUCCESS) {
			printf("Unable to open serial port for VISCA\n");
			return;
		}
		interface->broadcast = 0;
		if (VISCA_set_address(interface, &camera_count) != VISCA_SUCCESS) {
			printf("Unable to set address for VISCA\n");
			return;
		}
		printf("VISCA Camera count: %i\n", camera_count);
	}
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
