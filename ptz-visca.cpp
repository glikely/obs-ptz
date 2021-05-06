/* Pan Tilt Zoom visca instance
 *
 * Copyright 2020 Grant Likely <grant.likely@secretlab.ca>
 *
 * SPDX-License-Identifier: GPLv2
 */

#include "ptz-visca.hpp"
#include <QSerialPort>
#include <util/base.h>

/*
 * ViscaInterface wrapper class for VISCAInterface_t C structure
 */
class ViscaInterface {
private:
	static std::map<std::string, ViscaInterface*> interfaces;
	int camera_count;
	std::string uart_name;
	QSerialPort uart;

public:
	VISCAInterface_t iface;

	ViscaInterface(std::string &uart) : uart_name(uart) { iface.data = this; open(); }
	void open();
	void close();
	uint32_t write_packet_data(VISCAPacket_t *packet);
	int read_bytes(unsigned char *buffer, size_t size);

	static ViscaInterface *get_interface(std::string uart);
};

std::map<std::string, ViscaInterface*> ViscaInterface::interfaces;

void ViscaInterface::open()
{
	iface.reply_packet = NULL;
	iface.ipacket.length = 0;
	iface.busy = 0;
	iface.port_fd = 0;

	uart.setPortName(QString::fromStdString(uart_name));
	uart.setBaudRate(9600);
	if (!uart.open(QIODevice::ReadWrite)) {
		blog(LOG_INFO, "VISCA Unable to open UART %s", uart_name.c_str());
		return;
	}

	if (VISCA_set_address(&iface, &camera_count) != VISCA_SUCCESS) {
		camera_count = 0;
	}
	blog(LOG_INFO, "VISCA Interface on %s: %i camera%s found", uart_name.c_str(),
		camera_count, camera_count == 1 ? "" : "s");
}

void ViscaInterface::close()
{
	if (uart.isOpen())
		uart.close();
	camera_count = 0;
}

uint32_t ViscaInterface::write_packet_data(VISCAPacket_t *packet)
{
	if (!uart.isOpen())
		return VISCA_FAILURE;
	if (uart.write(packet->bytes, packet->length) < packet->length)
		return VISCA_FAILURE;
	return VISCA_SUCCESS;
}

extern "C" uint32_t _VISCA_write_packet_data(VISCAInterface_t *iface, VISCAPacket_t *packet)
{
	ViscaInterface *visca = (ViscaInterface *)iface->data;
	return visca->write_packet_data(packet);
}

int ViscaInterface::read_bytes(unsigned char *buffer, size_t size)
{
	if (!uart.isOpen())
		return VISCA_FAILURE;
	return uart.read((char *)buffer, size);
}

extern "C" int _VISCA_read_bytes(VISCAInterface_t *iface, unsigned char *buffer, size_t size)
{
	ViscaInterface *visca = (ViscaInterface *)iface->data;
	return visca->read_bytes(buffer, size);
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
	VISCA_set_pantilt_stop(&iface->iface, &camera);
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
