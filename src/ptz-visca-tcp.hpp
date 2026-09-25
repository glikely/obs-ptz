/* Pan Tilt Zoom VISCA over TCP implementation
 *
 * Copyright 2021 Grant Likely <grant.likely@secretlab.ca>
 *
 * SPDX-License-Identifier: GPLv2+
 */
#pragma once

#include <QObject>
#include <QTcpSocket>
#include "ptz-visca.hpp"

class ViscaTCPTransport : public ViscaTransport {
	Q_OBJECT

private:
	QTcpSocket visca_socket;
	QByteArray rxbuffer;
	QString host;
	int port = 5678;

	void receive_datagram(const QByteArray &packet);

private slots:
	void connectSocket();
	void on_socket_stateChanged(QAbstractSocket::SocketState);
	void poll();

public:
	ViscaTCPTransport();

	QString description(unsigned int address) const override;
	void update(OBSData config) override;
	void save(OBSData config) const override;
	void send(const QByteArray &msg, unsigned int address) override;

	static void add_obs_properties(obs_properties_t *props);
};
