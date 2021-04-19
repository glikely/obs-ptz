/* Pan Tilt Zoom visca instance
 *
 * Copyright 2020 Grant Likely <grant.likely@secretlab.ca>
 *
 * SPDX-License-Identifier: GPLv2
 */

#include "ptz-visca.hpp"

/*
 * ViscaInterface wrapper class for VISCAInterface_t C structure
 */
class ViscaInterface {
private:
	static std::map<std::string, ViscaInterface*> interfaces;
	int camera_count;
	std::string uart_name;

public:
	VISCAInterface_t iface;

	ViscaInterface(std::string &uart) : uart_name(uart) { open(); }
	void open();
	void close();

	static ViscaInterface *get_interface(std::string uart);
};

std::map<std::string, ViscaInterface*> ViscaInterface::interfaces;

void ViscaInterface::open()
{
	if (VISCA_open_serial(&iface, uart_name.c_str()) != VISCA_SUCCESS) {
		qDebug() << "Unable to open" << uart_name.c_str() << "for VISCA";
		return;
	}
	if (VISCA_set_address(&iface, &camera_count) != VISCA_SUCCESS) {
		qDebug() << "Unable to set address for VISCA";
		return;
	}
	qDebug() << "VISCA Camera count:" << camera_count;
}

void ViscaInterface::close()
{
	camera_count = 0;
	VISCA_close_serial(&iface);
}

ViscaInterface * ViscaInterface::get_interface(std::string uart)
{
	ViscaInterface *iface;
	qDebug() << "Looking for UART object" << uart.c_str();
	iface = interfaces[uart];
	if (!iface) {
		qDebug() << "Creating new VISCA object" << uart.c_str();
		iface = new ViscaInterface(uart);
		interfaces[uart] = iface;
	}
	return iface;
}

/*
 * PTZVisca Methods
 */
PTZVisca::PTZVisca(const char *uart_name, int address)
	: PTZDevice("visca")
{
	iface = ViscaInterface::get_interface(uart_name);
	camera.address = address;
	init();
}

PTZVisca::PTZVisca(obs_data_t *config)
	: PTZDevice("visca")
{
	set_config(config);
	init();
}

PTZVisca::~PTZVisca()
{
	pantilt_stop();
	zoom_stop();
}

void PTZVisca::init()
{
	VISCA_clear(&iface->iface, &camera);
	VISCA_get_camera_info(&iface->iface, &camera);
}

void PTZVisca::set_config(obs_data_t *config)
{
	PTZDevice::set_config(config);
	const char *uart = obs_data_get_string(config, "port");
	camera.address = obs_data_get_int(config, "address");
	if (uart)
		iface = ViscaInterface::get_interface(uart);
}

obs_data_t * PTZVisca::get_config()
{
	obs_data_set_int(config, "address", camera.address);
	return PTZDevice::get_config();
}

void PTZVisca::pantilt(double pan, double tilt)
{
	VISCA_set_pantilt(&iface->iface, &camera, pan * 10, tilt * 10);
}

void PTZVisca::pantilt_stop()
{
	VISCA_set_pantilt_stop(&iface->iface, &camera, 0, 0);
}

void PTZVisca::pantilt_home()
{
	VISCA_set_pantilt_home(&iface->iface, &camera);
}

#define ptzvisca_zoom_wrapper(dir) \
	void PTZVisca::zoom_##dir() \
	{ VISCA_set_zoom_##dir(&iface->iface, &camera); }
ptzvisca_zoom_wrapper(stop)
ptzvisca_zoom_wrapper(tele)
ptzvisca_zoom_wrapper(wide)
