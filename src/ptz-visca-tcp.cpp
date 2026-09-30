/* Pan Tilt Zoom VISCA over TCP implementation
 *
 * Copyright 2021 Grant Likely <grant.likely@secretlab.ca>
 *
 * SPDX-License-Identifier: GPLv2
 */

#include <qt-wrappers.hpp>
#include "ptz-visca-tcp.hpp"

ViscaTCPTransport::ViscaTCPTransport()
{
	visca_socket.setSocketOption(QAbstractSocket::KeepAliveOption, 1);
	connect(&visca_socket, &QTcpSocket::readyRead, this, &ViscaTCPTransport::poll);
	connect(&visca_socket, &QTcpSocket::stateChanged, this, &ViscaTCPTransport::on_socket_stateChanged);
}

QString ViscaTCPTransport::description(unsigned int address) const
{
	Q_UNUSED(address);
	return QString(obs_module_text("PTZ.Visca.TCP.HostPortName")).arg(effectiveHost(), QString::number(port));
}

void ViscaTCPTransport::connectSocket()
{
	if (!effectiveHost().isEmpty())
		visca_socket.connectToHost(effectiveHost(), port);
}

void ViscaTCPTransport::on_socket_stateChanged(QAbstractSocket::SocketState state)
{
	switch (state) {
	case QAbstractSocket::UnconnectedState:
		/* Attempt reconnection periodically */
		QTimer::singleShot(1900, this, &ViscaTCPTransport::connectSocket);
		break;
	case QAbstractSocket::ConnectedState:
		blog(LOG_INFO, "VISCA_over_TCP %s:%i connected", qPrintable(effectiveHost()), port);
		emit reset();
		break;
	default:
		break;
	}
}

void ViscaTCPTransport::flush()
{
	if (visca_socket.state() == QAbstractSocket::ConnectedState)
		visca_socket.waitForBytesWritten(100);
}

void ViscaTCPTransport::send(const QByteArray &msg, unsigned int address)
{
	Q_UNUSED(address);
	if (visca_socket.state() == QAbstractSocket::UnconnectedState)
		connectSocket();
	visca_socket.write(msg);
}

void ViscaTCPTransport::receive_datagram(const QByteArray &packet)
{
	int camera_count = 0;
	if (packet.size() < 3)
		return;
	if ((packet[1] & 0xf0) == VISCA_RESPONSE_ADDRESS) {
		switch (packet[1] & 0x0f) { /* Decode Packet Socket Field */
		case 0:
			camera_count = (packet[2] & 0x7) - 1;
			blog(LOG_INFO, "VISCA-over-TCP Interface %i camera%s found", camera_count,
			     camera_count == 1 ? "" : "s");
			emit reset();
			break;
		case 8:
			/* network change, trigger a change */
			visca_socket.write(VISCA_ENUMERATE.cmd);
			break;
		default:
			break;
		}
		return;
	}
	emit receive(packet);
}

void ViscaTCPTransport::poll()
{
	for (auto b : visca_socket.readAll()) {
		rxbuffer += b;
		if ((b & 0xff) == 0xff) {
			if (rxbuffer.size())
				receive_datagram(rxbuffer);
			rxbuffer.clear();
		}
	}
}

void ViscaTCPTransport::setSourceHost(const QString &new_host)
{
	if (new_host == source_host)
		return;
	bool followed = host.isEmpty();
	source_host = new_host;
	/* Drops the connection to the old address; reconnects on its own */
	if (followed)
		visca_socket.abort();
}

void ViscaTCPTransport::update(OBSData config)
{
	host = obs_data_get_string(config, "host");
	port = (int)obs_data_get_int(config, "tcp_port");
	if (!port)
		port = (int)obs_data_get_int(config, "port"); /* legacy schema */
	if (!port)
		port = 5678;

	connectSocket();
}

void ViscaTCPTransport::save(OBSData config) const
{
	obs_data_set_string(config, "host", QT_TO_UTF8(host));
	/* Greyed-out text in a blank Host field: where it will connect instead */
	obs_data_set_default_string(config, "host:placeholder", QT_TO_UTF8(source_host));
	obs_data_set_int(config, "tcp_port", port);
	obs_data_set_int(config, "port", port); /* legacy schema */
}

void ViscaTCPTransport::add_obs_properties(obs_properties_t *props)
{
	obs_property_set_long_description(
		obs_properties_add_text(props, "host", obs_module_text("PTZ.Device.Hostname"), OBS_TEXT_DEFAULT),
		obs_module_text("PTZ.Device.HostnameHint"));
	obs_properties_add_int(props, "tcp_port", obs_module_text("PTZ.Device.TCPPort"), 1, 65535, 1);
}
