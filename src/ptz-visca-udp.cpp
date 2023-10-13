/* Pan Tilt Zoom VISCA over UDP implementation
 *
 * Copyright 2021 Grant Likely <grant.likely@secretlab.ca>
 *
 * SPDX-License-Identifier: GPLv2
 */

#include <QNetworkDatagram>
#include "ptz-visca-udp.hpp"

std::map<int, ViscaUDPSocket *> ViscaUDPSocket::interfaces;

ViscaUDPSocket::ViscaUDPSocket(int port) : visca_port(port)
{
	if (!visca_socket.bind(QHostAddress::Any, visca_port)) {
		blog(LOG_INFO, "VISCA-over-IP bind to port %i failed",
		     visca_port);
		return;
	}
	connect(&visca_socket, &QUdpSocket::readyRead, this,
		&ViscaUDPSocket::poll);
}

void ViscaUDPSocket::receive_datagram(QNetworkDatagram &dg)
{
	QByteArray data = dg.data();
	if (disable_sequence) {
	    // devices which don't work with udp sequence most likely don't reply properly either
		// but we'll try anyway...
		int s = data.size();
		data = QByteArray::fromHex("0111000000000000") + dg.data();
		data[3] = s;
	
	}
	int type = ((data[0] & 0xff) << 8) | (data[1] & 0xff);
	int size = data[2] << 8 | data[3];
	
	if ((data.size() != size + 8) || size < 1) {
		blog(ptz_debug_level, "VISCA UDP (malformed) <-- %s",
		     qPrintable(data.toHex(':')));
		return;
	}

	switch (type) {
	case 0x0111:
		emit receive(data.mid(8, size));
		break;
	case 0x0200:
	case 0x0201: /* Check for sequence number out of sync */
		if (data[8] == (char)0x0f && data[8 + 1] == (char)1)
			emit reset();
		break;
	default:
		blog(ptz_debug_level, "VISCA UDP unrecognized type: %x", type);
	}
}

void ViscaUDPSocket::send(QHostAddress ip_address, const QByteArray &packet)
{
	visca_socket.writeDatagram(packet, ip_address, visca_port);
}

void ViscaUDPSocket::poll()
{
	while (visca_socket.hasPendingDatagrams()) {
		QNetworkDatagram dg = visca_socket.receiveDatagram();
		receive_datagram(dg);
	}
}

ViscaUDPSocket *ViscaUDPSocket::get_interface(int port)
{
	ViscaUDPSocket *iface;
	blog(ptz_debug_level, "Looking for Visca UDP Socket object %i", port);
	iface = interfaces[port];
	if (!iface) {
		blog(ptz_debug_level, "Creating new VISCA object %i", port);
		iface = new ViscaUDPSocket(port);
		interfaces[port] = iface;
	}
	return iface;
}

PTZViscaOverIP::PTZViscaOverIP(OBSData config) : PTZVisca(config), iface(NULL)
{
	address = 1;
	set_config(config);
}

PTZViscaOverIP::~PTZViscaOverIP()
{
	attach_interface(nullptr);
}

QString PTZViscaOverIP::description()
{
	return QString("VISCA/UDP %1:%2")
		.arg(ip_address.toString(), QString::number(iface->port()));
}

void PTZViscaOverIP::attach_interface(ViscaUDPSocket *new_iface)
{
	if (iface)
		iface->disconnect(this);
	iface = new_iface;
	if (iface) {
		connect(iface, &ViscaUDPSocket::receive, this,
			&PTZViscaOverIP::receive);
		connect(iface, &ViscaUDPSocket::reset, this,
			&PTZViscaOverIP::reset);
		reset();
	}
}

void PTZViscaOverIP::reset()
{
	sequence = 1;
	iface->send(ip_address, QByteArray::fromHex("020000010000000001"));
	cmd_get_camera_info();
}

void PTZViscaOverIP::send_immediate(const QByteArray &msg)
{

	if (iface->disable_sequence) {
		iface->send(ip_address, msg);
		return;
	}

	QByteArray p = QByteArray::fromHex("0100000000000000") + msg;
	p[1] = (0x9 == msg[1]) ? 0x10 : 0x00;
	p[3] = msg.size();
	p[4] = (sequence >> 24) & 0xff;
	p[5] = (sequence >> 16) & 0xff;
	p[6] = (sequence >> 8) & 0xff;
	p[7] = sequence & 0xff;
	p[8] = '\x81';
	sequence++;
	iface->send(ip_address, p);
}

void PTZViscaOverIP::set_config(OBSData config)
{
	PTZVisca::set_config(config);
	const char *ip = obs_data_get_string(config, "address");
	if (ip)
		ip_address = QHostAddress(ip);
	int port = (int)obs_data_get_int(config, "port");
	if (!port)
		port = 52381;

	attach_interface(ViscaUDPSocket::get_interface(port));
	iface->disable_sequence = obs_data_get_bool(config, "disable_sequence");
}

OBSData PTZViscaOverIP::get_config()
{
	OBSData config = PTZVisca::get_config();
	obs_data_set_string(config, "address",
			    qPrintable(ip_address.toString()));
	obs_data_set_int(config, "port", iface->port());
	obs_data_set_bool(config, "disable_sequence", iface->disable_sequence);
	return config;
}

obs_properties_t *PTZViscaOverIP::get_obs_properties()
{
	obs_properties_t *ptz_props = PTZVisca::get_obs_properties();
	obs_property_t *p = obs_properties_get(ptz_props, "interface");
	obs_properties_t *config = obs_property_group_content(p);
	obs_property_set_description(p, "VISCA (UDP) Connection");
	obs_properties_add_text(config, "address", "IP Address",
				OBS_TEXT_DEFAULT);
	obs_properties_add_int(config, "port", "UDP port", 1, 65535, 1);
	obs_properties_add_bool(config, "disable_sequence",
				"Disable Sequence" );
	return ptz_props;
}
