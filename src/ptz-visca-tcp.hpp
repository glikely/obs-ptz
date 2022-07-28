/* Pan Tilt Zoom VISCA over TCP implementation
 *
 * Copyright 2021 Grant Likely <grant.likely@secretlab.ca>
 *
 * SPDX-License-Identifier: GPLv2
 */
#pragma once

#include <QObject>
#include <QTcpSocket>
#include "ptz-visca.hpp"

class PTZViscaOverTCP : public PTZVisca {
	Q_OBJECT

private:
	QTcpSocket visca_socket;
	QByteArray rxbuffer;
	QString host;
	int port;

protected:
	void send_immediate(const QByteArray &msg);
	void reset();
	void receive_datagram(const QByteArray &packet);
	void poll();

private slots:
	void connectSocket();
	void on_socket_stateChanged(QAbstractSocket::SocketState);

public:
	PTZViscaOverTCP(OBSData config);

	void set_config(OBSData ptz_data);
	OBSData get_config();
	obs_properties_t *get_obs_properties();
};
