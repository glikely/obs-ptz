/* Pan Tilt Zoom Controls - ONVIF camera detection with WS-Discovery
 *
 * Copyright 2026 Jonatã Bolzan Loss <jonata@jonata.org>
 * Copyright 2026 Grant Likely <grant.likely@secretlab.ca>
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#pragma once

#include <QHostAddress>
#include <QSet>
#include <QTimer>
#include <QUdpSocket>
#include "ptz-discovery-kind.hpp"

/**
 * Finds ONVIF cameras by sending a WS-Discovery Probe for a
 * NetworkVideoTransmitter to the multicast group, from every interface that
 * can reach one, and taking each ProbeMatch that comes back within the
 * timeout as a camera.
 */
class PTZOnvifDiscovery : public QObject {
	Q_OBJECT

public:
	explicit PTZOnvifDiscovery(std::shared_ptr<PTZDiscoveryReporter> reporter);
	/* Looks for a while, reporting each device once, then says it has
	 * finished; again, if called again */
	void start();
	/* Gives up early, saying it has finished if it hadn't */
	void stop();

private:
	void sendProbe();
	void readReplies();
	void parseProbeMatches(const QByteArray &payload, const QHostAddress &sender);

	QUdpSocket m_socket;
	QTimer m_timer;
	QSet<QString> m_seen;
	bool m_running = false;
	std::shared_ptr<PTZDiscoveryReporter> m_reporter;
};

/* Registers its discovery kind, "ca.secretlab.obs-ptz.onvif.discovery" */
void ptz_onvif_register_discovery();
