/* Pan Tilt Zoom IPC365 implementation
 *
 * Copyright 2022 Jonatã Bolzan Loss <jonata@jonata.org>
 *
 * SPDX-License-Identifier: GPLv2
 */

#include "imported/qt-wrappers.hpp"
#include <QTimer>
#include <QtEndian>
#include "ptz-ipc365.hpp"

PTZIPC365::PTZIPC365(OBSData config)
	: PTZDevice(config)
{
	set_config(config);
	auto_settings_filter += {"port", "host"};
	tcp_socket.setSocketOption(QAbstractSocket::KeepAliveOption, 1);
	connect(&tcp_socket, &QTcpSocket::stateChanged, this, &PTZIPC365::on_socket_stateChanged);
}


void PTZIPC365::pantilt(double pan_, double tilt_)
{
	if (!(pan_ == 0.0 && tilt_ == 0.0)) {
		QByteArray cmd;

		cmd.append(QByteArray::fromHex("ccddeeff774f0000e31269004800000000000000af93c63b09f74b01010000000000000000000000"));

		if (pan_ < 0) {
			int pan = 255 - ((pan_ * -1) * 10);
			cmd.append(pan);
			cmd.append(QByteArray::fromHex("ffffff"));
		} else {
			int pan = pan_ * 10;
			cmd.append(pan);
			cmd.append(QByteArray::fromHex("000000"));
		}

		if (tilt_ < 0) {
			int tilt = 255 - ((tilt_ * -1) * 10);
			cmd.append(tilt);
			cmd.append(QByteArray::fromHex("ffffff"));
		} else {
			int tilt = tilt_ * 10;
			cmd.append(tilt);
			cmd.append(QByteArray::fromHex("000000"));
		}

		cmd.append(QByteArray::fromHex("0000000000000000000000000000000000000000000000"));

		send(cmd);
	};
}

void PTZIPC365::pantilt_home()
{
	pantilt_abs(255, 255);
}

void PTZIPC365::pantilt_abs(int pan, int tilt)
{
	QByteArray cmd;
	cmd.append(QByteArray::fromHex("ccddeeff774f0000e31269004800000000000000af93c63b09f74b01010000000000000000000000"));
	cmd.append(pan);
	cmd.append(QByteArray::fromHex("000000"));
	cmd.append(tilt);
	cmd.append(QByteArray::fromHex("000000"));
	cmd.append(QByteArray::fromHex("0000000000000000000000000000000000000000000000"));
	send(cmd);
}

void PTZIPC365::connectSocket()
{
	tcp_socket.connectToHost(host, port);
}

void PTZIPC365::on_socket_stateChanged(QAbstractSocket::SocketState state)
{
	blog(LOG_INFO, "IPC365 socket state: %s", qPrintable(QVariant::fromValue(state).toString()));
	switch (state) {
	case QAbstractSocket::UnconnectedState:
		/* Attempt reconnection periodically */
		QTimer::singleShot(900, this, SLOT(connectSocket));
		break;
	case QAbstractSocket::ConnectedState:
		blog(LOG_INFO, "IPC365 %s connected", QT_TO_UTF8(objectName()));
		// reset();s
		break;
	default:
		break;
	}
}

void PTZIPC365::send(const QByteArray &msg)
{
	if (tcp_socket.state() == QAbstractSocket::UnconnectedState)
		connectSocket();
	ptz_debug("IPC365 --> %s", qPrintable(msg.toHex()));
	tcp_socket.write(msg);
}


void PTZIPC365::set_config(OBSData config)
{
	PTZDevice::set_config(config);
	host = obs_data_get_string(config, "host");
	port = obs_data_get_int(config, "port");
	if (!port)
		port = 23456;
	connectSocket();
}

OBSData PTZIPC365::get_config()
{
	OBSData config = PTZDevice::get_config();
	obs_data_set_string(config, "host", QT_TO_UTF8(host));
	obs_data_set_int(config, "port", port);
	return config;
}

obs_properties_t *PTZIPC365::get_obs_properties()
{
	obs_properties_t *props = PTZDevice::get_obs_properties();
	obs_property_t *p = obs_properties_get(props, "interface");
	obs_properties_t *config = obs_property_group_content(p);
	obs_property_set_description(p, "IPC365 Connection");
	obs_properties_add_text(config, "host", "IP Host", OBS_TEXT_DEFAULT);
	obs_properties_add_int(config, "port", "TCP port", 1, 65535, 1);
	return props;
}
