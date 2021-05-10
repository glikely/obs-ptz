/* Pan Tilt Zoom visca instance
 *
 * Copyright 2020 Grant Likely <grant.likely@secretlab.ca>
 *
 * SPDX-License-Identifier: GPLv2
 */

#include "ptz-visca.hpp"
#include <util/base.h>
#include "libvisca.h"

std::map<std::string, ViscaInterface*> ViscaInterface::interfaces;

void ViscaInterface::open()
{
	camera_count = 0;

	uart.setPortName(QString::fromStdString(uart_name));
	uart.setBaudRate(9600);
	if (!uart.open(QIODevice::ReadWrite)) {
		blog(LOG_INFO, "VISCA Unable to open UART %s", uart_name.c_str());
		return;
	}

	connect(&uart, &QSerialPort::readyRead, this, &ViscaInterface::poll);

	cmd_enumerate();
}

void ViscaInterface::cmd_enumerate()
{
	const char msg[] = { '\x88', 0x30, 0x01, '\xff' };
	send(QByteArray::fromRawData(msg, sizeof(msg)));
}

void ViscaInterface::close()
{
	if (uart.isOpen())
		uart.close();
	camera_count = 0;
}

void ViscaInterface::send(const QByteArray &packet)
{
	if (!uart.isOpen())
		return;
	blog(LOG_INFO, "VISCA --> %s", packet.toHex(':').data());
	uart.write(packet);
}

void ViscaInterface::receive(const QByteArray &packet)
{
	blog(LOG_INFO, "VISCA <-- %s", packet.toHex(':').data());
	if (packet.size() < 3)
		return;
	switch (packet[1] & 0xf0) { /* Decode response field */
	case VISCA_RESPONSE_ADDRESS:
		switch (packet[1] & 0x0f) { /* Decode Packet Socket Field */
		case 0:
			camera_count = (packet[2] & 0x7) - 1;
			blog(LOG_INFO, "VISCA Interface %s: %i camera%s found", uart_name.c_str(),
				camera_count, camera_count == 1 ? "" : "s");
			break;
		case 8:
			/* network change, trigger a change */
			cmd_enumerate();
			break;
		default:
			break;
		}
		break;
	case VISCA_RESPONSE_ACK:
		/* Don't do anything with the ack yet */
		blog(LOG_DEBUG, "VISCA received ACK");
		emit receive_ack(packet);
		break;
	case VISCA_RESPONSE_COMPLETED:
		blog(LOG_DEBUG, "VISCA received complete");
		emit receive_complete(packet);
		break;
	case VISCA_RESPONSE_ERROR:
		blog(LOG_DEBUG, "VISCA received error");
		emit receive_error(packet);
		break;
	default:
		blog(LOG_INFO, "VISCA received unknown");
		/* Unknown */
		break;
	}
}

void ViscaInterface::poll()
{
	const QByteArray data = uart.readAll();
	for (int i = 0; i < data.size(); i++) {
		rxbuffer += data[i];
		if ((data[i] & 0xff) == 0xff) {
			if (rxbuffer.size())
				receive(rxbuffer);
			rxbuffer.clear();
		}
	}
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
	attach_interface(ViscaInterface::get_interface(uart_name));
}

PTZVisca::PTZVisca(obs_data_t *config)
	: PTZDevice("visca"), iface(NULL)
{
	set_config(config);
}

PTZVisca::~PTZVisca()
{
	pantilt(0, 0);
	zoom_stop();
}

void PTZVisca::send_pending()
{
	if (active_cmd.size() != 0 || pending_cmds.isEmpty())
		return;
	active_cmd = pending_cmds.takeFirst();
	iface->send(active_cmd);
}

void PTZVisca::send(QByteArray &msg)
{
	msg[0] = (char)(0x80 | address & 0x7);
	pending_cmds.append(msg);
	send_pending();
}

void PTZVisca::cmd_clear()
{
	const char msg[] = { 0, 0x01, 0x00, 0x01, '\xff' };
	send(QByteArray::fromRawData(msg, sizeof(msg)));
}

void PTZVisca::cmd_get_camera_info()
{
	const char msg[] = { 0, 0x09, 0x00, 0x02, '\xff' };
	send(QByteArray::fromRawData(msg, sizeof(msg)));
}

void PTZVisca::reset()
{
	cmd_clear();
	cmd_get_camera_info();
}

void PTZVisca::attach_interface(ViscaInterface *new_iface)
{
	if (iface)
		iface->disconnect(this);
	iface = new_iface;
	if (iface) {
		connect(iface, &ViscaInterface::receive_ack, this, &PTZVisca::receive_ack);
		connect(iface, &ViscaInterface::receive_complete, this, &PTZVisca::receive_complete);
	}
	reset();
}

void PTZVisca::set_config(obs_data_t *config)
{
	PTZDevice::set_config(config);
	const char *uart = obs_data_get_string(config, "port");
	address = obs_data_get_int(config, "address");
	if (!uart)
		return;

	blog(LOG_INFO, "PTZVisca::set_config() %p iface=%p", this, iface);
	attach_interface(ViscaInterface::get_interface(uart));
}

obs_data_t * PTZVisca::get_config()
{
	obs_data_set_int(config, "address", address);
	return PTZDevice::get_config();
}

void PTZVisca::receive_ack(const QByteArray &msg)
{
	if (VISCA_PACKET_SENDER(msg) != address)
		return;
	active_cmd.clear();
	send_pending();
}

void PTZVisca::receive_complete(const QByteArray &msg)
{
	blog(LOG_INFO, "VISCA %p receive_complete slot: %s", this, msg.toHex(':').data());
	if (VISCA_PACKET_SENDER(msg) != address || active_cmd.size() < 3)
		return;
	switch (active_cmd[2] & 0xff) {
	case VISCA_DEVICE_INFO:
		blog(LOG_INFO, "VISCA device info received: %s", msg.toHex(':').data());
		break;
	default:
		break;
	}
	active_cmd.clear();
	send_pending();
}

void PTZVisca::pantilt(double pan, double tilt)
{
	int pan_speed = pan;
	int tilt_speed = tilt;
	char msg[] = {
		0,
		VISCA_COMMAND,
		VISCA_CATEGORY_PAN_TILTER,
		VISCA_PT_DRIVE,
		0,
		0,
		VISCA_PT_DRIVE_HORIZ_STOP,
		VISCA_PT_DRIVE_VERT_STOP,
		'\xff' };

	if (pan != 0) {
		msg[4] = (char)abs(10 * pan);
		msg[6] = pan < 0 ? VISCA_PT_DRIVE_HORIZ_LEFT : VISCA_PT_DRIVE_HORIZ_RIGHT;
	}
	if (tilt != 0) {
		msg[5] = (char)abs(10 * tilt);
		msg[7] = tilt < 0 ? VISCA_PT_DRIVE_VERT_DOWN : VISCA_PT_DRIVE_VERT_UP;
	}
	send(QByteArray::fromRawData(msg, sizeof(msg)));
}

void PTZVisca::pantilt_stop()
{
	pantilt(0, 0);
}

void PTZVisca::pantilt_home()
{
	const char msg[] = {
		0,
		VISCA_COMMAND,
		VISCA_CATEGORY_PAN_TILTER,
		VISCA_PT_HOME,
		'\xff' };
	send(QByteArray::fromRawData(msg, sizeof(msg)));
}

void PTZVisca::zoom_tele()
{
	const char msg[] = {
		0,
		VISCA_COMMAND,
		VISCA_CATEGORY_CAMERA1,
		VISCA_ZOOM,
		VISCA_ZOOM_TELE,
		'\xff' };
	send(QByteArray::fromRawData(msg, sizeof(msg)));
}

void PTZVisca::zoom_wide()
{
	const char msg[] = {
		0,
		VISCA_COMMAND,
		VISCA_CATEGORY_CAMERA1,
		VISCA_ZOOM,
		VISCA_ZOOM_WIDE,
		'\xff' };
	send(QByteArray::fromRawData(msg, sizeof(msg)));
}

void PTZVisca::zoom_stop()
{
	const char msg[] = {
		0,
		VISCA_COMMAND,
		VISCA_CATEGORY_CAMERA1,
		VISCA_ZOOM,
		VISCA_ZOOM_STOP,
		'\xff' };
	send(QByteArray::fromRawData(msg, sizeof(msg)));
}
