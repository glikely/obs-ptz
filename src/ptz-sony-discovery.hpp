/* Pan Tilt Zoom Controls - Sony VISCA-over-IP camera detection
 *
 * Copyright 2026 Grant Likely <grant.likely@secretlab.ca>
 *
 * SPDX-License-Identifier: GPLv2
 */
#pragma once

#include <QSet>
#include <QTimer>
#include <QUdpSocket>
#include "ptz-discovery.hpp"

/**
 * Finds Sony VISCA-over-IP cameras with the "Setting Protocol" of Sony's
 * VISCA over IP documentation ("IP Related Setting Command", in the
 * SRG-120DH's command list, say): an "ENQ:network" inquiry broadcast to UDP
 * port 52380, which each camera answers with its MAC and IP address, model
 * and name, broadcast back to the same port.
 */
class PTZSonyDiscovery : public PTZDiscovery {
	Q_OBJECT

public:
	explicit PTZSonyDiscovery(QObject *parent = nullptr);
	void start() override;
	void stop() override;

private:
	void sendInquiry();
	void readReplies();
	void parseReply(const QByteArray &payload);

	QUdpSocket m_socket;
	QTimer m_timer;
	QSet<QString> m_seen;
	bool m_running = false;
};

void ptz_sony_register_discovery();
