/* Pan Tilt Zoom VISCA over UDP implementation
 *
 * Copyright 2021 Grant Likely <grant.likely@secretlab.ca>
 *
 * SPDX-License-Identifier: GPLv2
 */

#include <qt-wrappers.hpp>
#include <QHostInfo>
#include <QNetworkDatagram>
#include "ptz-visca-udp.hpp"

std::map<int, ViscaUDPSocket *> ViscaUDPSocket::interfaces;

ViscaUDPSocket::ViscaUDPSocket(int port) : visca_port(port)
{
	if (!visca_socket.bind(QHostAddress::Any, visca_port))
		/* Warn if bind failed; but continue anyway
		 * - Some cameras want the sender to use the same port.
		 * - Others don't care */
		blog(LOG_INFO, "VISCA-over-IP bind to port %i failed", visca_port);
	connect(&visca_socket, &QUdpSocket::readyRead, this, &ViscaUDPSocket::poll);
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
	blog(LOG_DEBUG, "Looking for Visca UDP Socket object %i", port);
	iface = interfaces[port];
	if (!iface) {
		blog(LOG_DEBUG, "Creating new VISCA object %i", port);
		iface = new ViscaUDPSocket(port);
		interfaces[port] = iface;
	}
	return iface;
}

/*
 * ViscaUDPTransport
 */
ViscaUDPTransport::~ViscaUDPTransport()
{
	attach_interface(nullptr);
}

QString ViscaUDPTransport::description(unsigned int address) const
{
	Q_UNUSED(address);
	return QString(obs_module_text("PTZ.Visca.UDP.HostPortName"))
		.arg(ip_address.toString(), QString::number(iface ? iface->port() : 0));
}

void ViscaUDPTransport::attach_interface(ViscaUDPSocket *new_iface)
{
	if (iface)
		iface->disconnect(this);
	iface = new_iface;
	if (iface) {
		connect(iface, &ViscaUDPSocket::receive_datagram, this, &ViscaUDPTransport::receive_datagram);
		protocol_reset();
	}
}

void ViscaUDPTransport::protocol_reset()
{
	for (int i = 0; i < 8; i++)
		seq_state[i] = 0;
	emit statIncrement("visca_udp_reset_count");
	if (iface)
		iface->send(ip_address, QByteArray::fromHex("020000010000000001"));
	emit reset();
}

void ViscaUDPTransport::receive_datagram(const QNetworkDatagram &dg)
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
		blog(LOG_DEBUG, "VISCA UDP (too small) <-- %s", qPrintable(data.toHex(':')));
		return;
	}
	uint16_t type = (uint8_t)data[0] << 8 | (uint8_t)data[1];
	/*uint16_t size = (uint8_t)data[2] << 8 | (uint8_t)data[3];*/
	uint32_t seq = (uint8_t)data[4] << 24 | (uint8_t)data[5] << 16 | (uint8_t)data[6] << 8 | (uint8_t)data[7];
	uint8_t reply_code = data[9] & 0x70;
	uint8_t slot = data[9] & 0x0f;

	switch (type) {
	case 0x0111:
		if (seq != seq_state[0] && seq != seq_state[slot]) {
			blog(LOG_DEBUG, "VISCA UDP out of seq; %i != [0]%i or [%i]%i) <-- %s", seq, seq_state[0], slot,
			     seq_state[slot], qPrintable(data.toHex(':')));
			emit statIncrement("visca_udp_outofseq_cmplt_count");
			return;
		}
		/* if slot is nonzero, update or clear the sequence number for that slot */
		if (slot)
			seq_state[slot] = (reply_code == 0x40) ? seq_state[0] : 0;
		emit receive(data.sliced(8));
		break;
	case 0x0200:
	case 0x0201: /* Check for sequence number out of sync */
		if (data[8] == (char)0x0f && data[8 + 1] == (char)1)
			protocol_reset();
		else if (data[8] == 0x01)
			emit refresh();
		break;
	default:
		blog(LOG_DEBUG, "VISCA UDP unrecognized type: %x", type);
	}
}

void ViscaUDPTransport::send(const QByteArray &msg, unsigned int address)
{
	Q_UNUSED(address);
	if (!iface)
		return;

	if (quirk_visca_udp_no_seq) {
		// Don't prepend the sequence field
		iface->send(ip_address, msg);
		emit statIncrement("visca_udp_sent_count");
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
	emit statIncrement("visca_udp_sent_count");
}

void ViscaUDPTransport::lookup_host_callback(const QHostInfo info)
{
	auto addrs = info.addresses();
	if (addrs.isEmpty())
		return;
	auto new_addr = addrs.first();
	if (new_addr != ip_address) {
		ip_address = new_addr;
		protocol_reset();
	}
}

void ViscaUDPTransport::update(OBSData config)
{
	QString new_host = obs_data_get_string(config, "host");
	int port;
	if (obs_data_has_user_value(config, "udp_port"))
		port = obs_data_get_int(config, "udp_port");
	else
		port = obs_data_get_int(config, "port"); /* legacy schema */
	if (new_host != host) {
		ip_address.clear();
		host = new_host;
		if (!host.isEmpty()) {
			bool is_ip = ip_address.setAddress(host);
			if (!is_ip)
				QHostInfo::lookupHost(host, this, &ViscaUDPTransport::lookup_host_callback);
		}
	}
	if (!port)
		port = 52381;
	attach_interface(ViscaUDPSocket::get_interface(port));
	quirk_visca_udp_no_seq = obs_data_get_bool(config, "quirk_visca_udp_no_seq");
}

void ViscaUDPTransport::save(OBSData config) const
{
	obs_data_set_string(config, "host", qPrintable(host));
	obs_data_set_int(config, "udp_port", iface ? iface->port() : 0);
	obs_data_set_int(config, "port", iface ? iface->port() : 0); /* legacy schema */
	obs_data_set_bool(config, "quirk_visca_udp_no_seq", quirk_visca_udp_no_seq);
}

void ViscaUDPTransport::add_obs_properties(obs_properties_t *props)
{
	obs_properties_add_text(props, "host", obs_module_text("PTZ.Device.Hostname"), OBS_TEXT_DEFAULT);
	obs_properties_add_int(props, "udp_port", obs_module_text("PTZ.Device.UDPPort"), 1, 65535, 1);
	obs_properties_add_bool(props, "quirk_visca_udp_no_seq", obs_module_text("PTZ.Visca.UDP.QuirkNoSeq"));
}
