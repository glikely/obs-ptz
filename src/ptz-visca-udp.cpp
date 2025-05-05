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
		blog(LOG_INFO, "VISCA-over-IP bind to port %i failed", visca_port);
		return;
	}
	connect(&visca_socket, &QUdpSocket::readyRead, this, &ViscaUDPSocket::poll);
}

void PTZViscaOverIP::receive_datagram(const QNetworkDatagram &dg)
{
	if (!dg.senderAddress().isEqual(ip_address)) // Check if this packet is for us.
		return;

	QByteArray data = dg.data();
	if (quirk_visca_udp_no_seq) {
		// Prepend an empty sequence field
		int s = data.size();
		data = QByteArray::fromHex("0111000000000000") + dg.data();
		data[3] = s;
	}
	if (data.size() < 9) {
		blog(ptz_debug_level, "VISCA UDP (too small) <-- %s", qPrintable(data.toHex(':')));
		return;
	}
	uint16_t type = (uint8_t)data[0] << 8 | (uint8_t)data[1];
	uint16_t size = (uint8_t)data[2] << 8 | (uint8_t)data[3];
	uint32_t seq = (uint8_t)data[4] << 24 | (uint8_t)data[5] << 16 | (uint8_t)data[6] << 8 | (uint8_t)data[7];
	uint8_t reply_code = data[9] & 0x70;
	uint8_t slot = data[9] & 0x0f;

	if ((data.size() != size + 8) || size < 1) {
		blog(ptz_debug_level, "VISCA UDP (malformed) <-- %s", qPrintable(data.toHex(':')));
		incrementStatistic("visca_udp_malformed_count");
		return;
	}

	switch (type) {
	case 0x0111:
		switch (reply_code) {
		case 0x40:
			if (seq != seq_state[0]) {
				blog(ptz_debug_level, "VISCA UDP (out of seq %i != %i) <-- %s", seq, seq_state[0],
				     qPrintable(data.toHex(':')));
				incrementStatistic("visca_udp_outofseq_ack_count");
				return;
			}
			seq_state[slot] = seq_state[0];
			break;
		case 0x50:
			if (seq != seq_state[slot]) {
				blog(ptz_debug_level, "VISCA UDP (out of seq %i != %i) <-- %s", seq, seq_state[0],
				     qPrintable(data.toHex(':')));
				incrementStatistic("visca_udp_outofseq_cmplt_count");
				return;
			}
			break;
		default:
			blog(ptz_debug_level, "VISCA UDP (unknown) <-- %s", qPrintable(data.toHex(':')));
			incrementStatistic("visca_udp_unknown_count");
			return;
		}
		receive(data.mid(8, size));
		break;
	case 0x0200:
	case 0x0201: /* Check for sequence number out of sync */
		if (data[8] == (char)0x0f && data[8 + 1] == (char)1)
			reset();
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
	while (visca_socket.hasPendingDatagrams())
		emit receive_datagram(visca_socket.receiveDatagram());
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
	return QString("VISCA/UDP %1:%2").arg(ip_address.toString(), QString::number(iface->port()));
}

void PTZViscaOverIP::attach_interface(ViscaUDPSocket *new_iface)
{
	if (iface)
		iface->disconnect(this);
	iface = new_iface;
	if (iface) {
		connect(iface, SIGNAL(receive_datagram(QNetworkDatagram)), this,
			SLOT(receive_datagram(QNetworkDatagram)));
		reset();
	}
}

void PTZViscaOverIP::reset()
{
	for (int i = 0; i < 8; i++)
		seq_state[i] = 0;
	incrementStatistic("visca_udp_reset_count");
	iface->send(ip_address, QByteArray::fromHex("020000010000000001"));
	cmd_get_camera_info();
}

void PTZViscaOverIP::send_immediate(const QByteArray &msg)
{
	if (quirk_visca_udp_no_seq) {
		// Don't prepend the sequence field
		iface->send(ip_address, msg);
		incrementStatistic("visca_udp_sent_count");
		return;
	}
	QByteArray p = QByteArray::fromHex("0100000000000000") + msg;
	seq_state[0]++;
	p[1] = (0x9 == msg[1]) ? 0x10 : 0x00;
	p[3] = msg.size();
	p[4] = (seq_state[0] >> 24) & 0xff;
	p[5] = (seq_state[0] >> 16) & 0xff;
	p[6] = (seq_state[0] >> 8) & 0xff;
	p[7] = seq_state[0] & 0xff;
	p[8] = '\x81';
	iface->send(ip_address, p);
	incrementStatistic("visca_udp_sent_count");
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
	quirk_visca_udp_no_seq = obs_data_get_bool(config, "quirk_visca_udp_no_seq");
}

OBSData PTZViscaOverIP::get_config()
{
	OBSData config = PTZVisca::get_config();
	obs_data_set_string(config, "address", qPrintable(ip_address.toString()));
	obs_data_set_int(config, "port", iface->port());
	obs_data_set_bool(config, "quirk_visca_udp_no_seq", quirk_visca_udp_no_seq);
	return config;
}

obs_properties_t *PTZViscaOverIP::get_obs_properties()
{
	obs_properties_t *ptz_props = PTZVisca::get_obs_properties();
	obs_property_t *p = obs_properties_get(ptz_props, "interface");
	obs_properties_t *config = obs_property_group_content(p);
	obs_property_set_description(p, "VISCA (UDP) Connection");
	obs_properties_add_text(config, "address", "IP Address", OBS_TEXT_DEFAULT);
	obs_properties_add_int(config, "port", "UDP port", 1, 65535, 1);
	obs_properties_add_bool(config, "quirk_visca_udp_no_seq", "Don't use sequence numbers");
	return ptz_props;
}
