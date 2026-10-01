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

class ViscaTCPTransport : public ViscaTransport {
	Q_OBJECT

private:
	QTcpSocket visca_socket;
	QByteArray rxbuffer;
	QString host; /* configured; empty to follow the source's */
	QString source_host;
	QString effectiveHost() const { return host.isEmpty() ? source_host : host; }
	/* Datavideo's DVIP: each packet, both ways, after its length (and the
	 * length's own 2 bytes) in 2 bytes, big endian */
	bool dvip;
	int default_port() const { return dvip ? 5002 : 5678; }
	int port;

	void write(const QByteArray &packet);
	void receive_datagram(const QByteArray &packet);

private slots:
	void connectSocket();
	void on_socket_stateChanged(QAbstractSocket::SocketState);
	void poll();

public:
	explicit ViscaTCPTransport(bool dvip = false);

	QString description(unsigned int address) const override;
	void update(OBSData config) override;
	void save(OBSData config) const override;
	void send(const QByteArray &msg, unsigned int address) override;
	void flush() override;
	void setSourceHost(const QString &new_host) override;

	static void add_obs_properties(obs_properties_t *props);
};
