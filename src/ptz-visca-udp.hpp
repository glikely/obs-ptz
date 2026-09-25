/* Pan Tilt Zoom VISCA over UDP implementation
 *
 * Copyright 2021 Grant Likely <grant.likely@secretlab.ca>
 *
 * SPDX-License-Identifier: GPLv2+
 */
#pragma once

#include <QObject>
#include <QHostInfo>
#include <QUdpSocket>
#include "ptz-visca.hpp"

class ViscaUDPSocket : public QObject {
	Q_OBJECT

private:
	/* Global lookup table of UART instances, used to eliminate duplicates */
	static std::map<int, ViscaUDPSocket *> interfaces;

	int visca_port;
	QUdpSocket visca_socket;

signals:
	void receive_datagram(const QNetworkDatagram &dg);

public:
	ViscaUDPSocket(int port = 52381);
	void send(QHostAddress ip_address, const QByteArray &packet);
	int port() { return visca_port; }

	static ViscaUDPSocket *get_interface(int port);

public slots:
	void poll();
};

class ViscaUDPTransport : public ViscaTransport {
	Q_OBJECT

private:
	uint32_t seq_state[8] = {};
	QString host;
	QHostAddress ip_address;
	ViscaUDPSocket *iface = nullptr;
	bool quirk_visca_udp_no_seq = false;
	void attach_interface(ViscaUDPSocket *iface);
	/* Clears the sequence state and (re-)sends the VISCA-over-IP reset
	 * datagram, then signals PTZVisca to re-query the camera */
	void protocol_reset();

public:
	ViscaUDPTransport() = default;
	~ViscaUDPTransport() override;

	QString description(unsigned int address) const override;
	void update(OBSData config) override;
	void save(OBSData config) const override;
	void send(const QByteArray &msg, unsigned int address) override;

	static void add_obs_properties(obs_properties_t *props);

public slots:
	void receive_datagram(const QNetworkDatagram &datagram);
	void lookup_host_callback(const QHostInfo hostinfo);
};
