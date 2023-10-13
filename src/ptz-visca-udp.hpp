/* Pan Tilt Zoom VISCA over UDP implementation
 *
 * Copyright 2021 Grant Likely <grant.likely@secretlab.ca>
 *
 * SPDX-License-Identifier: GPLv2
 */
#pragma once

#include <QObject>
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
	void receive(const QByteArray &packet);
	void reset();

public:
	ViscaUDPSocket(int port = 52381);
	void receive_datagram(QNetworkDatagram &datagram);
	void send(QHostAddress ip_address, const QByteArray &packet);
	int port() { return visca_port; }
	bool quirk_visca_udp_no_seq;

	static ViscaUDPSocket *get_interface(int port);

public slots:
	void poll();
};

class PTZViscaOverIP : public PTZVisca {
	Q_OBJECT

private:
	int sequence;
	QHostAddress ip_address;
	ViscaUDPSocket *iface;
	void attach_interface(ViscaUDPSocket *iface);

protected:
	void send_immediate(const QByteArray &msg);
	void reset();

public:
	PTZViscaOverIP(OBSData config);
	~PTZViscaOverIP();
	virtual QString description();

	void set_config(OBSData ptz_data);
	OBSData get_config();
	obs_properties_t *get_obs_properties();
};
