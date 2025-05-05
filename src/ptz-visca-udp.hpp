/* Pan Tilt Zoom VISCA over UDP implementation
 *
 * Copyright 2021 Grant Likely <grant.likely@secretlab.ca>
 *
 * SPDX-License-Identifier: GPLv2
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

class PTZViscaOverIP : public PTZVisca {
	Q_OBJECT

private:
	uint32_t seq_state[8];
	QString host;
	QHostAddress ip_address;
	ViscaUDPSocket *iface;
	bool quirk_visca_udp_no_seq;
	void attach_interface(ViscaUDPSocket *iface);

protected:
	void send_immediate(const QByteArray &msg);
	void reset();

public slots:
	void receive_datagram(const QNetworkDatagram &datagram);
	void lookup_host_callback(const QHostInfo hostinfo);

public:
	PTZViscaOverIP(OBSData config);
	~PTZViscaOverIP();
	virtual QString description();

	void set_config(OBSData ptz_data);
	OBSData get_config();
	obs_properties_t *get_obs_properties();
};
