/* Pan Tilt Zoom VISCA over Serial UART implementation
 *
 * Copyright 2021 Grant Likely <grant.likely@secretlab.ca>
 *
 * SPDX-License-Identifier: GPLv2+
 */

#include <qt-wrappers.hpp>
#include "ptz-visca-uart.hpp"

std::map<QString, ViscaUART *> ViscaUART::interfaces;

const PTZCmd VISCA_IF_CLEAR("88010010ff");

ViscaUART::ViscaUART(QString &port_name) : PTZUARTWrapper(port_name)
{
	camera_count = 0;
}

bool ViscaUART::open()
{
	camera_count = 0;
	bool rc = PTZUARTWrapper::open();
	if (rc)
		send(VISCA_ENUMERATE.cmd);
	return rc;
}

void ViscaUART::receive_datagram(const QByteArray &packet)
{
	if (packet.size() < 3)
		return;
	if ((packet[1] & 0xf0) == VISCA_RESPONSE_ADDRESS) {
		switch (packet[1] & 0x0f) { /* Decode Packet Socket Field */
		case 0:
			camera_count = (packet[2] & 0x7) - 1;
			blog(LOG_INFO, "VISCA Interface %s: %i camera%s found", qPrintable(portName()), camera_count,
			     camera_count == 1 ? "" : "s");
			send(VISCA_IF_CLEAR.cmd);
			emit reset();
			break;
		case 1:
			// Response from IF_CLEAR message; ignore
			break;
		case 8:
			/* network change, trigger a change */
			send(VISCA_ENUMERATE.cmd);
			break;
		default:
			break;
		}
		return;
	}

	emit receive(packet);
}

void ViscaUART::receiveBytes(const QByteArray &msg)
{
	for (auto b : msg) {
		rxbuffer += b;
		if ((b & 0xff) == 0xff) {
			if (rxbuffer.size())
				receive_datagram(rxbuffer);
			rxbuffer.clear();
		}
	}
}

ViscaUART *ViscaUART::get_interface(QString port_name)
{
	ViscaUART *iface;
	blog(LOG_DEBUG, "Looking for UART object %s", qPrintable(port_name));
	iface = interfaces[port_name];
	if (!iface) {
		blog(LOG_DEBUG, "Creating new VISCA object %s", qPrintable(port_name));
		iface = new ViscaUART(port_name);
		iface->open();
		interfaces[port_name] = iface;
	}
	return iface;
}

/*
 * ViscaSerialTransport
 */
ViscaSerialTransport::~ViscaSerialTransport()
{
	attach_interface(nullptr);
}

QString ViscaSerialTransport::description(unsigned int address) const
{
	return QString("VISCA %1 id:%2").arg(iface ? iface->portName() : QString(), QString::number(address));
}

void ViscaSerialTransport::attach_interface(ViscaUART *new_iface)
{
	if (iface)
		iface->disconnect(this);
	iface = new_iface;
	if (iface) {
		connect(iface, &ViscaUART::receive, this, &ViscaTransport::receive);
		connect(iface, &ViscaUART::reset, this, &ViscaTransport::reset);
	}
}

void ViscaSerialTransport::send(const QByteArray &msg_, unsigned int address)
{
	if (!iface)
		return;
	QByteArray msg = msg_;
	msg[0] = (char)(0x80 | (address & 0x7)); // Set the camera address
	iface->send(msg);
}

void ViscaSerialTransport::update(OBSData config)
{
	const char *uart = obs_data_get_string(config, "serial_port");
	if (!uart || !*uart)
		uart = obs_data_get_string(config, "port"); /* legacy schema */
	if (!uart || !*uart)
		return;

	ViscaUART *ifc = ViscaUART::get_interface(uart);
	ifc->setConfig(config);
	attach_interface(ifc);
}

void ViscaSerialTransport::save(OBSData config) const
{
	if (iface)
		iface->save(config);
}

void ViscaSerialTransport::add_obs_properties(obs_properties_t *props)
{
	PTZUARTWrapper::addOBSProperties(props);
	obs_properties_add_int(props, "address", obs_module_text("PTZ.Visca.ID"), 1, 7, 1);
}
