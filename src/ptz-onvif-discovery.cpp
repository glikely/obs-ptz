/* Pan Tilt Zoom Controls - ONVIF camera detection with WS-Discovery
 *
 * Copyright 2026 Jonatã Bolzan Loss <jonata@jonata.org>
 * Copyright 2026 Grant Likely <grant.likely@secretlab.ca>
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include "ptz-onvif-discovery.hpp"

#include <QNetworkDatagram>
#include <QNetworkInterface>
#include <QRegularExpression>
#include <QUrl>
#include <QUuid>
#include <QtXml/QDomDocument>
#include "ptz.h"

/* WS-Discovery's multicast group, and how long cameras have to answer */
static const QHostAddress WSD_MULTICAST_ADDR("239.255.255.250");
static constexpr quint16 WSD_PORT = 3702;
static constexpr int WSD_TIMEOUT_MS = 3000;

static const char *WSD_NS = "http://schemas.xmlsoap.org/ws/2005/04/discovery";
static const char *WSA_NS = "http://schemas.xmlsoap.org/ws/2004/08/addressing";

static const char *PROBE_TEMPLATE =
	"<?xml version=\"1.0\" encoding=\"UTF-8\"?>"
	"<s:Envelope"
	" xmlns:s=\"http://www.w3.org/2003/05/soap-envelope\""
	" xmlns:a=\"http://schemas.xmlsoap.org/ws/2004/08/addressing\""
	" xmlns:d=\"http://schemas.xmlsoap.org/ws/2005/04/discovery\""
	" xmlns:dn=\"http://www.onvif.org/ver10/network/wsdl\">"
	"<s:Header>"
	"<a:Action s:mustUnderstand=\"1\">http://schemas.xmlsoap.org/ws/2005/04/discovery/Probe</a:Action>"
	"<a:MessageID>uuid:%1</a:MessageID>"
	"<a:ReplyTo><a:Address>http://schemas.xmlsoap.org/ws/2004/08/addressing/role/anonymous</a:Address></a:ReplyTo>"
	"<a:To s:mustUnderstand=\"1\">urn:schemas-xmlsoap-org:ws:2005:04:discovery</a:To>"
	"</s:Header>"
	"<s:Body><d:Probe><d:Types>dn:NetworkVideoTransmitter</d:Types></d:Probe></s:Body>"
	"</s:Envelope>";

PTZOnvifDiscovery::PTZOnvifDiscovery(QObject *parent) : PTZDiscovery(parent)
{
	m_timer.setSingleShot(true);
	connect(&m_timer, &QTimer::timeout, this, &PTZOnvifDiscovery::stop);
	connect(&m_socket, &QUdpSocket::readyRead, this, &PTZOnvifDiscovery::readReplies);
}

void PTZOnvifDiscovery::start()
{
	if (m_running)
		return;
	m_seen.clear();
	m_running = true;

	/* Replies come back to the port the probe was sent from */
	if (m_socket.state() != QAbstractSocket::BoundState &&
	    !m_socket.bind(QHostAddress::AnyIPv4, 0, QUdpSocket::ShareAddress | QUdpSocket::ReuseAddressHint)) {
		blog(LOG_WARNING, "ONVIF discovery could not bind a socket: %s",
		     qUtf8Printable(m_socket.errorString()));
		stop();
		return;
	}
	sendProbe();
	m_timer.start(WSD_TIMEOUT_MS);
}

void PTZOnvifDiscovery::stop()
{
	if (!m_running)
		return;
	m_timer.stop();
	m_running = false;
	emit finished();
}

/* On every interface that can multicast, so a host on several networks
 * finds the cameras on all of them, not only the default route's */
void PTZOnvifDiscovery::sendProbe()
{
	QByteArray probe = QString(PROBE_TEMPLATE).arg(QUuid::createUuid().toString(QUuid::WithoutBraces)).toUtf8();
	bool sent = false;
	for (const auto &iface : QNetworkInterface::allInterfaces()) {
		auto flags = iface.flags();
		if (!(flags & QNetworkInterface::IsUp) || !(flags & QNetworkInterface::IsRunning) ||
		    (flags & QNetworkInterface::IsLoopBack) || !(flags & QNetworkInterface::CanMulticast))
			continue;
		bool ipv4 = false;
		for (const auto &entry : iface.addressEntries())
			ipv4 = ipv4 || entry.ip().protocol() == QAbstractSocket::IPv4Protocol;
		if (!ipv4)
			continue;
		m_socket.setMulticastInterface(iface);
		if (m_socket.writeDatagram(probe, WSD_MULTICAST_ADDR, WSD_PORT) == probe.size())
			sent = true;
	}
	if (!sent)
		m_socket.writeDatagram(probe, WSD_MULTICAST_ADDR, WSD_PORT);
}

void PTZOnvifDiscovery::readReplies()
{
	while (m_socket.hasPendingDatagrams()) {
		QNetworkDatagram datagram = m_socket.receiveDatagram();
		if (m_running)
			parseProbeMatches(datagram.data(), datagram.senderAddress());
	}
}

void PTZOnvifDiscovery::parseProbeMatches(const QByteArray &payload, const QHostAddress &sender)
{
	QDomDocument doc;
#if QT_VERSION >= QT_VERSION_CHECK(6, 8, 0)
	if (!doc.setContent(QAnyStringView(payload), QDomDocument::ParseOption::UseNamespaceProcessing))
		return;
#else
	if (!doc.setContent(payload, true))
		return;
#endif
	QString senderHost = QHostAddress(sender.toIPv4Address()).toString();
	const QRegularExpression space("\\s+");

	auto matches = doc.elementsByTagNameNS(WSD_NS, "ProbeMatch");
	for (int i = 0; i < matches.length(); i++) {
		QDomElement match = matches.at(i).toElement();

		QString endpoint;
		auto epr = match.elementsByTagNameNS(WSA_NS, "EndpointReference");
		if (epr.length() > 0) {
			auto address = epr.at(0).toElement().elementsByTagNameNS(WSA_NS, "Address");
			if (address.length() > 0)
				endpoint = address.at(0).toElement().text().trimmed();
		}

		/* The device service's address. There can be several; the
		 * one on the address that answered can be reached. A camera
		 * may advertise only an address it can't be reached on (from
		 * behind NAT, or on another subnet): then use its path on the
		 * address that answered, as other ONVIF clients do. */
		auto xaddrs = match.elementsByTagNameNS(WSD_NS, "XAddrs");
		if (xaddrs.length() == 0)
			continue;
		QStringList urls = xaddrs.at(0).toElement().text().split(space, Qt::SkipEmptyParts);
		if (urls.isEmpty())
			continue;
		QUrl url(urls.first());
		for (const auto &u : urls) {
			if (QUrl(u).host() == senderHost) {
				url = QUrl(u);
				break;
			}
		}
		if (url.host() != senderHost)
			url.setHost(senderHost);
		int port = url.port(80);

		QString id = endpoint.isEmpty() ? url.toString() : endpoint;
		if (m_seen.contains(id))
			continue;
		m_seen.insert(id);

		/* Scopes like onvif://www.onvif.org/name/MAKER and
		 * onvif://www.onvif.org/hardware/MODEL say what it is */
		QString maker, model;
		auto scopes = match.elementsByTagNameNS(WSD_NS, "Scopes");
		if (scopes.length() > 0) {
			for (const auto &scope : scopes.at(0).toElement().text().split(space, Qt::SkipEmptyParts)) {
				QString path = QUrl(scope).path();
				if (path.startsWith("/name/"))
					maker = QUrl::fromPercentEncoding(path.mid(6).toUtf8());
				else if (path.startsWith("/hardware/"))
					model = QUrl::fromPercentEncoding(path.mid(10).toUtf8());
			}
		}

		PTZDetectedDevice device;
		device.id = id;
		device.name = QStringList({maker, model}).join(' ').trimmed();
		if (device.name.isEmpty())
			device.name = "ONVIF";
		device.address = QString("%1:%2").arg(url.host()).arg(port);
		OBSDataAutoRelease settings = obs_data_create();
		obs_data_set_string(settings, "type", "onvif");
		obs_data_set_string(settings, "host", qUtf8Printable(url.host()));
		obs_data_set_int(settings, "port", port);
		device.settings = settings.Get();
		emit deviceFound(device);
	}
}

void ptz_onvif_register_discovery()
{
	ptz_discovery_register([](QObject *parent) -> PTZDiscovery * { return new PTZOnvifDiscovery(parent); });
}
