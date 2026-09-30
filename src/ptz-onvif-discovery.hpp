/* Pan Tilt Zoom Controls - ONVIF camera detection with WS-Discovery
 *
 * Copyright 2026 Jonatã Bolzan Loss <jonata@jonata.org>
 * Copyright 2026 Grant Likely <grant.likely@secretlab.ca>
 *
 * SPDX-License-Identifier: GPLv2
 */
#pragma once

#include <QHostAddress>
#include <QSet>
#include <QTimer>
#include <QUdpSocket>
#include "ptz-discovery.hpp"

/**
 * Finds ONVIF cameras by sending a WS-Discovery Probe for a
 * NetworkVideoTransmitter to the multicast group, from every interface that
 * can reach one, and taking each ProbeMatch that comes back within the
 * timeout as a camera.
 */
class PTZOnvifDiscovery : public PTZDiscovery {
	Q_OBJECT

public:
	explicit PTZOnvifDiscovery(QObject *parent = nullptr);
	void start() override;
	void stop() override;

private:
	void sendProbe();
	void readReplies();
	void parseProbeMatches(const QByteArray &payload, const QHostAddress &sender);

	QUdpSocket m_socket;
	QTimer m_timer;
	QSet<QString> m_seen;
	bool m_running = false;
};

void ptz_onvif_register_discovery();
