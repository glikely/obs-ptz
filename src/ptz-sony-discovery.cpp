/* Pan Tilt Zoom Controls - Sony VISCA-over-IP camera detection
 *
 * Copyright 2026 Grant Likely <grant.likely@secretlab.ca>
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include "ptz-sony-discovery.hpp"

#include <QHostAddress>
#include <QMap>
#include <QNetworkDatagram>
#include <QNetworkInterface>
#include "ptz.h"

/* The port the inquiry goes to and the replies come back on, the port
 * VISCA over IP itself uses, and how long cameras have to answer */
static constexpr quint16 SONY_SETUP_PORT = 52380;
static constexpr int SONY_VISCA_PORT = 52381;
static constexpr int SONY_TIMEOUT_MS = 3000;

/* Every message is framed by STX and ETX, and each field ends with 0xff */
static constexpr char STX = 0x02;
static constexpr char ETX = 0x03;
static constexpr char FIELD_END = (char)0xff;

PTZSonyDiscovery::PTZSonyDiscovery(QObject *parent) : PTZDiscovery(parent)
{
	m_timer.setSingleShot(true);
	connect(&m_timer, &QTimer::timeout, this, &PTZSonyDiscovery::stop);
	connect(&m_socket, &QUdpSocket::readyRead, this, &PTZSonyDiscovery::readReplies);
}

void PTZSonyDiscovery::start()
{
	if (m_running)
		return;
	m_seen.clear();
	m_running = true;

	/* Cameras broadcast their replies to the setup port, not back to the
	 * port the inquiry came from, so listen on it, alongside anything else
	 * on this host that does too. Some reply to the sender as well: failing
	 * the setup port, those can still be heard. */
	if (m_socket.state() != QAbstractSocket::BoundState &&
	    !m_socket.bind(QHostAddress::AnyIPv4, SONY_SETUP_PORT,
			   QUdpSocket::ShareAddress | QUdpSocket::ReuseAddressHint) &&
	    !m_socket.bind(QHostAddress::AnyIPv4, 0)) {
		blog(LOG_WARNING, "Sony VISCA discovery could not bind a socket: %s",
		     qUtf8Printable(m_socket.errorString()));
		stop();
		return;
	}
	sendInquiry();
	m_timer.start(SONY_TIMEOUT_MS);
}

void PTZSonyDiscovery::stop()
{
	if (!m_running)
		return;
	m_timer.stop();
	m_running = false;
	emit finished();
}

/* To the limited broadcast address, as the protocol says, and to each
 * interface's own broadcast address, since the limited one only goes out on
 * the default route's interface on some systems */
void PTZSonyDiscovery::sendInquiry()
{
	QByteArray inquiry;
	inquiry.append(STX).append("ENQ:network").append(FIELD_END).append(ETX);
	QList<QHostAddress> targets = {QHostAddress::Broadcast};
	for (const auto &iface : QNetworkInterface::allInterfaces()) {
		auto flags = iface.flags();
		if (!(flags & QNetworkInterface::IsUp) || !(flags & QNetworkInterface::IsRunning) ||
		    (flags & QNetworkInterface::IsLoopBack) || !(flags & QNetworkInterface::CanBroadcast))
			continue;
		for (const auto &entry : iface.addressEntries())
			if (entry.ip().protocol() == QAbstractSocket::IPv4Protocol && !entry.broadcast().isNull() &&
			    !targets.contains(entry.broadcast()))
				targets.append(entry.broadcast());
	}
	for (const auto &target : targets)
		m_socket.writeDatagram(inquiry, target, SONY_SETUP_PORT);
}

void PTZSonyDiscovery::readReplies()
{
	while (m_socket.hasPendingDatagrams()) {
		QNetworkDatagram datagram = m_socket.receiveDatagram();
		if (m_running)
			parseReply(datagram.data());
	}
}

/* An inquiry reply: STX, then "MAC:..", "MODEL:..", "SOFTVERSION:..",
 * "IPADR:..", "MASK:..", "GATEWAY:..", "NAME:.." and "WRITE:..", each ended
 * by 0xff, then ETX. Anything else on the port (inquiries, this one's own
 * included, and replies to network settings) has no MAC and IPADR both. */
void PTZSonyDiscovery::parseReply(const QByteArray &payload)
{
	if (payload.size() < 2 || payload.front() != STX || payload.back() != ETX)
		return;
	QMap<QString, QString> fields;
	for (const QByteArray &field : payload.mid(1, payload.size() - 2).split(FIELD_END)) {
		int colon = field.indexOf(':');
		if (colon > 0)
			fields[QString::fromLatin1(field.left(colon))] =
				QString::fromLatin1(field.mid(colon + 1)).trimmed();
	}
	QString mac = fields.value("MAC");
	QHostAddress ip(fields.value("IPADR"));
	if (mac.isEmpty() || ip.protocol() != QAbstractSocket::IPv4Protocol)
		return;
	QString id = "sony:" + mac.toLower();
	if (m_seen.contains(id))
		return;
	m_seen.insert(id);

	PTZDetectedDevice device;
	device.id = id;
	QString model = fields.value("MODEL");
	QString name = fields.value("NAME");
	device.name = name.isEmpty() ? model : model.isEmpty() || name == model ? name : name + " " + model;
	if (device.name.isEmpty())
		device.name = "Sony VISCA";
	device.address = ip.toString();
	OBSDataAutoRelease settings = obs_data_create();
	obs_data_set_string(settings, "type", "visca-over-ip");
	obs_data_set_string(settings, "host", qUtf8Printable(ip.toString()));
	obs_data_set_int(settings, "udp_port", SONY_VISCA_PORT);
	device.settings = settings.Get();
	emit deviceFound(device);
}

void ptz_sony_register_discovery()
{
	ptz_discovery_register([](QObject *parent) -> PTZDiscovery * { return new PTZSonyDiscovery(parent); });
}
