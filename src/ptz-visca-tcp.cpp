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
	return QString(obs_module_text("PTZ.Visca.TCP.HostPortName")).arg(host, QString::number(port));
}

void ViscaTCPTransport::connectSocket()
{
	visca_socket.connectToHost(host, port);
}

void ViscaTCPTransport::on_socket_stateChanged(QAbstractSocket::SocketState state)
{
	switch (state) {
	case QAbstractSocket::UnconnectedState:
		/* Attempt reconnection periodically */
		QTimer::singleShot(1900, this, &ViscaTCPTransport::connectSocket);
		break;
	case QAbstractSocket::ConnectedState:
		blog(LOG_INFO, "VISCA_over_TCP %s:%i connected", qPrintable(host), port);
		emit reset();
		break;
	default:
		break;
	}
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

void ViscaTCPTransport::update(OBSData config)
{
	host = obs_data_get_string(config, "host");
	if (obs_data_has_user_value(config, "tcp_port"))
		port = (int)obs_data_get_int(config, "tcp_port");
	else
		port = (int)obs_data_get_int(config, "port"); /* fallback to old config schema */
	connectSocket();
}

void ViscaTCPTransport::save(OBSData config) const
{
	obs_data_set_string(config, "host", QT_TO_UTF8(host));
	obs_data_set_int(config, "tcp_port", port);
	obs_data_set_int(config, "port", port); /* preserve older config schema */
}

void ViscaTCPTransport::add_obs_properties(obs_properties_t *props)
{
	obs_properties_add_text(props, "host", obs_module_text("PTZ.Device.Hostname"), OBS_TEXT_DEFAULT);
	obs_properties_add_int(props, "tcp_port", obs_module_text("PTZ.Device.TCPPort"), 1, 65535, 1);
}
