/* Pan Tilt Zoom VISCA over TCP implementation
 *
 * Copyright 2021 Grant Likely <grant.likely@secretlab.ca>
 *
 * SPDX-License-Identifier: GPLv2
 */

#include "imported/qt-wrappers.hpp"
#include "ptz-visca-tcp.hpp"

PTZViscaOverTCP::PTZViscaOverTCP(OBSData config) : PTZVisca(config)
{
	address = 1;
	set_config(config);
	visca_socket.setSocketOption(QAbstractSocket::KeepAliveOption, 1);
	connect(&visca_socket, &QTcpSocket::readyRead, this,
		&PTZViscaOverTCP::poll);
	connect(&visca_socket, &QTcpSocket::stateChanged, this,
		&PTZViscaOverTCP::on_socket_stateChanged);
}

QString PTZViscaOverTCP::description()
{
	return QString("VISCA/TCP %1:%2").arg(host, QString::number(port));
}

void PTZViscaOverTCP::reset()
{
	send(VISCA_Clear);
	cmd_get_camera_info();
}

void PTZViscaOverTCP::connectSocket()
{
	visca_socket.connectToHost(host, port);
}

void PTZViscaOverTCP::on_socket_stateChanged(QAbstractSocket::SocketState state)
{
	switch (state) {
	case QAbstractSocket::UnconnectedState:
		/* Attempt reconnection periodically */
		QTimer::singleShot(1900, this, SLOT(connectSocket()));
		break;
	case QAbstractSocket::ConnectedState:
		blog(LOG_INFO, "VISCA_over_TCP %s connected",
		     QT_TO_UTF8(objectName()));
		reset();
		break;
	default:
		break;
	}
}

void PTZViscaOverTCP::send_immediate(const QByteArray &msg)
{
	if (visca_socket.state() == QAbstractSocket::UnconnectedState)
		connectSocket();
	ptz_debug("VISCA_over_TCP --> %s", qPrintable(msg.toHex(':')));
	visca_socket.write(msg);
}

void PTZViscaOverTCP::receive_datagram(const QByteArray &packet)
{
	int camera_count = 0;
	ptz_debug("VISCA_over_TCP <-- %s", packet.toHex(':').data());
	if (packet.size() < 3)
		return;
	if ((packet[1] & 0xf0) == VISCA_RESPONSE_ADDRESS) {
		switch (packet[1] & 0x0f) { /* Decode Packet Socket Field */
		case 0:
			camera_count = (packet[2] & 0x7) - 1;
			blog(LOG_INFO,
			     "VISCA-over-TCP Interface %i camera%s found",
			     camera_count, camera_count == 1 ? "" : "s");
			send_immediate(VISCA_IF_CLEAR.cmd);
			reset();
			break;
		case 1:
			// Response from IF_CLEAR message; ignore
			break;
		case 8:
			/* network change, trigger a change */
			send_immediate(VISCA_ENUMERATE.cmd);
			break;
		default:
			break;
		}
		return;
	}
	receive(packet);
}

void PTZViscaOverTCP::poll()
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

void PTZViscaOverTCP::set_config(OBSData config)
{
	PTZDevice::set_config(config);
	host = obs_data_get_string(config, "host");
	port = (int)obs_data_get_int(config, "port");
	if (!port)
		port = 5678;
	connectSocket();
}

OBSData PTZViscaOverTCP::get_config()
{
	OBSData config = PTZDevice::get_config();
	obs_data_set_string(config, "host", QT_TO_UTF8(host));
	obs_data_set_int(config, "port", port);
	return config;
}

obs_properties_t *PTZViscaOverTCP::get_obs_properties()
{
	obs_properties_t *ptz_props = PTZVisca::get_obs_properties();
	obs_property_t *p = obs_properties_get(ptz_props, "interface");
	obs_properties_t *config = obs_property_group_content(p);
	obs_property_set_description(p, "VISCA (TCP) Connection");
	obs_properties_add_text(config, "host", "IP Host", OBS_TEXT_DEFAULT);
	obs_properties_add_int(config, "port", "TCP port", 1, 65535, 1);
	return ptz_props;
}
