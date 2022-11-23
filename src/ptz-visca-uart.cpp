/* Pan Tilt Zoom VISCA over Serial UART implementation
 *
 * Copyright 2021 Grant Likely <grant.likely@secretlab.ca>
 *
 * SPDX-License-Identifier: GPLv2
 */

#include "imported/qt-wrappers.hpp"
#include "ptz-visca-uart.hpp"

std::map<QString, ViscaUART *> ViscaUART::interfaces;

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

void ViscaUART::close()
{
	PTZUARTWrapper::close();
	camera_count = 0;
}

void ViscaUART::receive_datagram(const QByteArray &packet)
{
	ptz_debug("VISCA <-- %s", packet.toHex(':').data());
	if (packet.size() < 3)
		return;
	if ((packet[1] & 0xf0) == VISCA_RESPONSE_ADDRESS) {
		switch (packet[1] & 0x0f) { /* Decode Packet Socket Field */
		case 0:
			camera_count = (packet[2] & 0x7) - 1;
			blog(LOG_INFO, "VISCA Interface %s: %i camera%s found",
			     qPrintable(uart.portName()), camera_count,
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
	ptz_debug("Looking for UART object %s", qPrintable(port_name));
	iface = interfaces[port_name];
	if (!iface) {
		ptz_debug("Creating new VISCA object %s",
			  qPrintable(port_name));
		iface = new ViscaUART(port_name);
		iface->open();
		interfaces[port_name] = iface;
	}
	return iface;
}

PTZViscaSerial::PTZViscaSerial(OBSData config) : PTZVisca(config), iface(NULL)
{
	set_config(config);
}

PTZViscaSerial::~PTZViscaSerial()
{
	attach_interface(nullptr);
}

void PTZViscaSerial::attach_interface(ViscaUART *new_iface)
{
	if (iface)
		iface->disconnect(this);
	iface = new_iface;
	if (iface) {
		connect(iface, &ViscaUART::receive, this,
			&PTZViscaSerial::receive);
		connect(iface, &ViscaUART::reset, this, &PTZViscaSerial::reset);
	}
}

void PTZViscaSerial::reset()
{
	cmd_get_camera_info();
}

void PTZViscaSerial::send_immediate(const QByteArray &msg_)
{
	QByteArray msg = msg_;
	msg[0] = (char)(0x80 | (address & 0x7)); // Set the camera address
	iface->send(msg);
}

void PTZViscaSerial::set_config(OBSData config)
{
	PTZDevice::set_config(config);
	const char *uart = obs_data_get_string(config, "port");
	address = qBound(1, (int)obs_data_get_int(config, "address"), 7);
	if (!uart)
		return;

	iface = ViscaUART::get_interface(uart);
	iface->setConfig(config);
	attach_interface(iface);
}

OBSData PTZViscaSerial::get_config()
{
	OBSData config = PTZDevice::get_config();
	obs_data_apply(config, iface->getConfig());
	obs_data_set_int(config, "address", address);
	return config;
}

obs_properties_t *PTZViscaSerial::get_obs_properties()
{
	obs_properties_t *ptz_props = PTZVisca::get_obs_properties();
	obs_property_t *p = obs_properties_get(ptz_props, "interface");
	obs_properties_t *config = obs_property_group_content(p);
	obs_property_set_description(p, "VISCA (Serial) Connection");

	iface->addOBSProperties(config);
	obs_properties_add_int(config, "address", "VISCA ID", 1, 7, 1);

	return ptz_props;
}
