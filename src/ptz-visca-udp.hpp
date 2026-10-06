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

class ViscaUDPTransport : public ViscaTransport {
	Q_OBJECT

private:
	/* Sequence number of the last request sent. A reply carries the
	 * sequence number of the request it answers, and is accepted for any of
	 * the last SEQ_WINDOW requests: a slow camera can answer a request after
	 * a retry has been sent. */
	static constexpr uint32_t SEQ_WINDOW = 16;
	uint32_t seq_last = 0;
	QString host;        /* configured; empty to follow the source's */
	QString source_host; /* from the parent source */
	QString active_host; /* what ip_address was resolved from */
	QHostAddress ip_address;
	ViscaUDPSocket *iface = nullptr;
	bool quirk_visca_udp_no_seq = false;
	void attach_interface(ViscaUDPSocket *iface);
	/* Clears the sequence state and (re-)sends the VISCA-over-IP reset
	 * datagram, then signals PTZVisca to re-query the camera */
	void protocol_reset();
	/* Resolve the configured host, or the source's if there isn't one */
	void apply_host();

public:
	ViscaUDPTransport() = default;
	~ViscaUDPTransport() override;

	void update(OBSData config) override;
	void save(OBSData config) const override;
	void saveDefaults(OBSData config) const override;
	void send(const QByteArray &msg, unsigned int address) override;
	void setSourceHost(const QString &new_host) override;
	/* Sony cameras were seen to drop about half of the requests that came
	 * within 3ms of their last reply, and almost none after 10ms */
	int minRequestGapMs() const override { return 15; }

	static void add_obs_properties(obs_properties_t *props);

public slots:
	void receive_datagram(const QNetworkDatagram &datagram);
	void lookup_host_callback(const QHostInfo hostinfo);
};
