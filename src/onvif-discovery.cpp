/* ONVIF WS-Discovery probe and selection dialog
 *
 * Copyright 2026 Jonatã Bolzan Loss <jonata@jonata.org>
 *
 * SPDX-License-Identifier: GPLv2
 */
#include "onvif-discovery.hpp"

#include <obs-module.h>
#include <util/base.h>

#include <QCryptographicHash>
#include <QDateTime>
#include <QCheckBox>
#include <QComboBox>
#include <QDialogButtonBox>
#include <QFile>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QHostAddress>
#include <QLabel>
#include <QLineEdit>
#include <QNetworkDatagram>
#include <QNetworkInterface>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QRegularExpression>
#include <QSplitter>
#include <QTableWidget>
#include <QTableWidgetItem>
#include <QTimeZone>
#include <QtXml/QDomDocument>
#include <QUrl>
#include <QUuid>
#include <QVBoxLayout>

/* WS-Discovery uses link-local multicast on this address/port */
static const QHostAddress WSD_MULTICAST_ADDR("239.255.255.250");
static constexpr quint16 WSD_PORT = 3702;

/* Some camera firmwares (Xiongmai/hsoap is the prolific offender) emit
 * unescaped '&' inside RTSP/URI text content, which is not valid XML and
 * makes Qt's QDomDocument refuse the whole document. Python's ElementTree
 * tolerates it but we can't change the parser. Replace any '&' that's not
 * already starting a recognized entity reference with '&amp;'. This is a
 * targeted fix for `&protocol=`, `&channel=`, `&stream=` in stream URIs
 * — none of our authored payloads contain bare ampersands. */
static QByteArray sanitizeXmlAmpersands(const QByteArray &xml)
{
	static const QRegularExpression re(QStringLiteral("&(?!(?:amp|lt|gt|quot|apos|#[0-9]+|#x[0-9a-fA-F]+);)"));
	QString s = QString::fromUtf8(xml);
	s.replace(re, "&amp;");
	return s.toUtf8();
}

static const char *kProbeTemplate =
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
	"<s:Body>"
	"<d:Probe>"
	"<d:Types>dn:NetworkVideoTransmitter</d:Types>"
	"</d:Probe>"
	"</s:Body>"
	"</s:Envelope>";

OnvifDiscovery::OnvifDiscovery(QObject *parent) : QObject(parent)
{
	m_timer.setSingleShot(true);
	connect(&m_timer, &QTimer::timeout, this, &OnvifDiscovery::onTimeout);
	connect(&m_socket, &QUdpSocket::readyRead, this, &OnvifDiscovery::onReadyRead);
}

void OnvifDiscovery::start(int timeoutMs)
{
	if (m_running)
		return;
	m_seenUuids.clear();
	m_running = true;

	/* Bind to an ephemeral port on all interfaces so we can receive
	 * unicast responses sent back to our source port. ShareAddress lets
	 * multiple processes on the host discover concurrently. */
	if (m_socket.state() != QAbstractSocket::BoundState) {
		if (!m_socket.bind(QHostAddress::AnyIPv4, 0, QUdpSocket::ShareAddress | QUdpSocket::ReuseAddressHint)) {
			emit errorOccurred(m_socket.errorString());
			m_running = false;
			emit finished();
			return;
		}
	}

	sendProbe();
	m_timer.start(timeoutMs);
}

void OnvifDiscovery::stop()
{
	if (!m_running)
		return;
	m_timer.stop();
	m_running = false;
	emit finished();
}

void OnvifDiscovery::sendProbe()
{
	m_messageId = QUuid::createUuid().toString(QUuid::WithoutBraces);
	QByteArray probe = QString(kProbeTemplate).arg(m_messageId).toUtf8();

	/* Send on each suitable interface so multi-homed hosts find cameras
	 * on every LAN, not only the default-route one. */
	bool sentAny = false;
	const auto interfaces = QNetworkInterface::allInterfaces();
	for (const auto &iface : interfaces) {
		auto flags = iface.flags();
		if (!(flags & QNetworkInterface::IsUp) || !(flags & QNetworkInterface::IsRunning))
			continue;
		if (flags & QNetworkInterface::IsLoopBack)
			continue;
		if (!(flags & QNetworkInterface::CanMulticast))
			continue;

		bool hasIPv4 = false;
		for (const auto &entry : iface.addressEntries()) {
			if (entry.ip().protocol() == QAbstractSocket::IPv4Protocol) {
				hasIPv4 = true;
				break;
			}
		}
		if (!hasIPv4)
			continue;

		m_socket.setMulticastInterface(iface);
		qint64 written = m_socket.writeDatagram(probe, WSD_MULTICAST_ADDR, WSD_PORT);
		if (written == probe.size())
			sentAny = true;
	}

	if (!sentAny) {
		/* Fall back to the OS default route */
		m_socket.writeDatagram(probe, WSD_MULTICAST_ADDR, WSD_PORT);
	}
}

void OnvifDiscovery::onReadyRead()
{
	while (m_socket.hasPendingDatagrams()) {
		auto dgram = m_socket.receiveDatagram();
		parseProbeMatch(dgram.data(), dgram.senderAddress());
	}
}

void OnvifDiscovery::onTimeout()
{
	m_running = false;
	emit finished();
}

void OnvifDiscovery::parseProbeMatch(const QByteArray &payload, const QHostAddress &sender)
{
	QDomDocument doc;
#if QT_VERSION >= QT_VERSION_CHECK(6, 8, 0)
	QDomDocument::ParseOptions options = QDomDocument::ParseOption::UseNamespaceProcessing;
	if (!doc.setContent(payload, options))
		return;
#else
	if (!doc.setContent(payload, true))
		return;
#endif

	const QString nsDisco("http://schemas.xmlsoap.org/ws/2005/04/discovery");
	const QString nsAddr("http://schemas.xmlsoap.org/ws/2004/08/addressing");

	auto matches = doc.elementsByTagNameNS(nsDisco, "ProbeMatch");
	for (int i = 0; i < matches.length(); i++) {
		auto m = matches.at(i).toElement();

		OnvifCameraInfo info;

		auto epr = m.elementsByTagNameNS(nsAddr, "EndpointReference");
		if (epr.length() > 0) {
			auto addr = epr.at(0).toElement().elementsByTagNameNS(nsAddr, "Address");
			if (addr.length() > 0)
				info.uuid = addr.at(0).toElement().text().trimmed();
		}

		auto xaddrs = m.elementsByTagNameNS(nsDisco, "XAddrs");
		if (xaddrs.length() > 0) {
			/* XAddrs may contain multiple URLs separated by spaces.
			 * Prefer one whose host matches the IP that actually
			 * answered this probe — that's by definition reachable
			 * from us. If none match (a common Xiongmai/budget-cam
			 * quirk where the camera advertises its DHCP/static IP
			 * even when reached via NAT or a different subnet), keep
			 * the first XAddr's path but rewrite its host to the
			 * responder. ODM and most other ONVIF clients do this. */
			QStringList urls =
				xaddrs.at(0).toElement().text().split(QRegularExpression("\\s+"), Qt::SkipEmptyParts);
			QString chosen;
			for (const auto &u : urls) {
				QUrl url(u);
				if (url.host() == sender.toString()) {
					chosen = u;
					break;
				}
			}
			if (chosen.isEmpty() && !urls.isEmpty()) {
				QUrl rewrite(urls.first());
				rewrite.setHost(sender.toString());
				chosen = rewrite.toString();
			}
			info.xaddr = chosen;
		}

		if (info.xaddr.isEmpty())
			continue;

		QUrl url(info.xaddr);
		info.host = url.host();
		info.port = url.port(80);

		auto typesEl = m.elementsByTagNameNS(nsDisco, "Types");
		if (typesEl.length() > 0)
			info.types =
				typesEl.at(0).toElement().text().split(QRegularExpression("\\s+"), Qt::SkipEmptyParts);

		auto scopesEl = m.elementsByTagNameNS(nsDisco, "Scopes");
		if (scopesEl.length() > 0)
			info.scopes =
				scopesEl.at(0).toElement().text().split(QRegularExpression("\\s+"), Qt::SkipEmptyParts);

		/* Extract manufacturer / model / name / location from scopes.
		 * Scopes look like onvif://www.onvif.org/hardware/MODEL or
		 * onvif://www.onvif.org/name/MANUFACTURER. */
		for (const auto &scope : info.scopes) {
			QUrl s(scope);
			QString path = s.path();
			QString value = QUrl::fromPercentEncoding(s.path().toUtf8());
			if (path.startsWith("/hardware/"))
				info.model = QUrl::fromPercentEncoding(path.mid(10).toUtf8());
			else if (path.startsWith("/name/"))
				info.manufacturer = QUrl::fromPercentEncoding(path.mid(6).toUtf8());
			else if (path.startsWith("/location/"))
				info.location = QUrl::fromPercentEncoding(path.mid(10).toUtf8());
		}
		if (info.name.isEmpty() && !info.manufacturer.isEmpty() && !info.model.isEmpty())
			info.name = info.manufacturer + " " + info.model;
		else if (info.name.isEmpty())
			info.name = info.manufacturer.isEmpty() ? info.host : info.manufacturer;

		QString dedupeKey = !info.uuid.isEmpty() ? info.uuid : info.xaddr;
		if (m_seenUuids.contains(dedupeKey))
			continue;
		m_seenUuids.insert(dedupeKey);

		emit cameraFound(info);
	}
}

/* ------------------------------------------------------------------------ */
/* OnvifMediaProbe                                                          */
/* ------------------------------------------------------------------------ */

namespace {
const QString kNsSoap = "http://www.w3.org/2003/05/soap-envelope";
const QString kNsAddrSoap = "http://www.w3.org/2005/08/addressing";
const QString kNsOnvifDevice = "http://www.onvif.org/ver10/device/wsdl";
const QString kNsOnvifMedia = "http://www.onvif.org/ver10/media/wsdl";
const QString kNsOnvifSchema = "http://www.onvif.org/ver10/schema";
const QString kNsWsse = "http://docs.oasis-open.org/wss/2004/01/oasis-200401-wss-wssecurity-secext-1.0.xsd";
const QString kNsWsu = "http://docs.oasis-open.org/wss/2004/01/oasis-200401-wss-wssecurity-utility-1.0.xsd";
const QString kPasswordDigestType =
	"http://docs.oasis-open.org/wss/2004/01/oasis-200401-wss-username-token-profile-1.0#PasswordDigest";
const QString kBase64BinaryEnc =
	"http://docs.oasis-open.org/wss/2004/01/oasis-200401-wss-soap-message-security-1.0#Base64Binary";

QString envelope(const QString &security, const QString &action, const QString &body)
{
	return QStringLiteral("<?xml version=\"1.0\" encoding=\"UTF-8\"?>"
			      "<s:Envelope xmlns:s=\"%1\" xmlns:wsa=\"%2\" xmlns:wsse=\"%3\" "
			      "xmlns:wsu=\"%4\" xmlns:tds=\"%5\" xmlns:trt=\"%6\" xmlns:tt=\"%7\">"
			      "<s:Header>"
			      "<wsa:Action s:mustUnderstand=\"1\">%8</wsa:Action>"
			      "%9"
			      "</s:Header>"
			      "<s:Body>%10</s:Body>"
			      "</s:Envelope>")
		.arg(kNsSoap, kNsAddrSoap, kNsWsse, kNsWsu, kNsOnvifDevice, kNsOnvifMedia, kNsOnvifSchema, action,
		     security, body);
}
} // namespace

OnvifMediaProbe::OnvifMediaProbe(QObject *parent) : QObject(parent)
{
	connect(&m_nam, &QNetworkAccessManager::finished, this, &OnvifMediaProbe::onReplyFinished);
}

void OnvifMediaProbe::fetch(const QString &host, int port, const QString &username, const QString &password)
{
	cancel();
	m_token++;
	m_host = host;
	m_port = port;
	m_username = username;
	m_password = password;
	m_deviceXAddr = QString("http://%1:%2/onvif/device_service").arg(host).arg(port);
	m_mediaXAddr.clear();
	m_profileTokens.clear();
	m_profileNames.clear();
	m_profileIdx = 0;
	m_results.clear();
	m_timeOffsetSecs = 0;
	m_step = Step::SystemDateAndTime;
	requestSystemDateAndTime();
}

void OnvifMediaProbe::cancel()
{
	m_step = Step::Idle;
	if (m_currentReply) {
		m_currentReply->disconnect(this);
		m_currentReply->abort();
		m_currentReply->deleteLater();
		m_currentReply = nullptr;
	}
}

QString OnvifMediaProbe::wsSecurityHeader() const
{
	QUuid nonceUuid = QUuid::createUuid();
	QByteArray nonceBytes = nonceUuid.toRfc4122();
	QString nonce64 = QString::fromLatin1(nonceBytes.toBase64());
	/* Apply per-camera clock offset so cameras whose clock differs from
	 * the local host (Xiongmai/hsoap firmwares are particularly strict)
	 * accept our timestamp. Offset is learned from the prior
	 * GetSystemDateAndTime call in the probe sequence. */
	QString timestamp = QDateTime::currentDateTimeUtc().addSecs(m_timeOffsetSecs).toString(Qt::ISODate);
	QByteArray hashInput = nonceBytes + timestamp.toUtf8() + m_password.toUtf8();
	QString digest = QString::fromLatin1(QCryptographicHash::hash(hashInput, QCryptographicHash::Sha1).toBase64());

	auto xmlEscape = [](const QString &s) {
		QString out = s;
		out.replace('&', "&amp;").replace('<', "&lt;").replace('>', "&gt;").replace('"', "&quot;");
		return out;
	};

	return QStringLiteral("<wsse:Security s:mustUnderstand=\"1\">"
			      "<wsse:UsernameToken>"
			      "<wsse:Username>%1</wsse:Username>"
			      "<wsse:Password Type=\"%2\">%3</wsse:Password>"
			      "<wsse:Nonce EncodingType=\"%4\">%5</wsse:Nonce>"
			      "<wsu:Created>%6</wsu:Created>"
			      "</wsse:UsernameToken>"
			      "</wsse:Security>")
		.arg(xmlEscape(m_username), kPasswordDigestType, digest, kBase64BinaryEnc, nonce64, timestamp);
}

void OnvifMediaProbe::sendRequest(const QString &url, const QString &action, const QString &body, bool authenticated)
{
	/* Skip WS-Security when no password is set — cameras like the user's
	 * XM530 firmware (and ODM by default) accept anonymous SOAP for the
	 * media/PTZ services, and sending a useless empty-password digest
	 * just makes them return a fault. */
	const bool needAuth = authenticated && !m_password.isEmpty();
	const QString security = needAuth ? wsSecurityHeader() : QString();
	QString xml = envelope(security, action, body);
	QNetworkRequest req((QUrl(url)));
	req.setHeader(QNetworkRequest::ContentTypeHeader, "application/soap+xml; charset=utf-8");
	/* Tight per-request timeout so failures surface to the user in seconds
	 * rather than waiting for the OS TCP timeout (~2 min). 10s is generous
	 * for an ONVIF SOAP call. */
	req.setTransferTimeout(10000);
	m_currentReply = m_nam.post(req, xml.toUtf8());
	m_currentReply->setProperty("probeToken", m_token);
}

void OnvifMediaProbe::requestSystemDateAndTime()
{
	/* Unauthenticated per spec; gives us the camera's clock for the rest
	 * of the probe's WS-Security timestamps. */
	sendRequest(m_deviceXAddr, kNsOnvifDevice + "/GetSystemDateAndTime", "<tds:GetSystemDateAndTime/>",
		    /*authenticated=*/false);
}

void OnvifMediaProbe::requestCapabilities()
{
	const QString body = "<tds:GetCapabilities><tds:Category>All</tds:Category></tds:GetCapabilities>";
	sendRequest(m_deviceXAddr, kNsOnvifDevice + "/GetCapabilities", body, /*authenticated=*/true);
}

void OnvifMediaProbe::requestProfiles()
{
	sendRequest(m_mediaXAddr, kNsOnvifMedia + "/GetProfiles", "<trt:GetProfiles/>", /*authenticated=*/true);
}

void OnvifMediaProbe::requestNextStreamUri()
{
	if (m_profileIdx >= m_profileTokens.size()) {
		m_step = Step::Idle;
		emit streamsReady(m_results);
		return;
	}
	const QString token = m_profileTokens.at(m_profileIdx);
	const QString body = QStringLiteral("<trt:GetStreamUri>"
					    "<trt:StreamSetup>"
					    "<tt:Stream>RTP-Unicast</tt:Stream>"
					    "<tt:Transport><tt:Protocol>RTSP</tt:Protocol></tt:Transport>"
					    "</trt:StreamSetup>"
					    "<trt:ProfileToken>%1</trt:ProfileToken>"
					    "</trt:GetStreamUri>")
				     .arg(token);
	sendRequest(m_mediaXAddr, kNsOnvifMedia + "/GetStreamUri", body, /*authenticated=*/true);
}

void OnvifMediaProbe::onReplyFinished(QNetworkReply *reply)
{
	int replyToken = reply->property("probeToken").toInt();
	reply->deleteLater();
	/* Aborts from cancel() and stale replies after a re-fetch must be
	 * dropped silently — the user expects "type new creds, see new result",
	 * not a transient "Operation canceled" flash. */
	if (reply->error() == QNetworkReply::OperationCanceledError)
		return;
	if (reply != m_currentReply || replyToken != m_token)
		return;
	m_currentReply = nullptr;

	if (reply->error() != QNetworkReply::NoError) {
		QString detail = reply->errorString();
		int status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
		if (status == 401 || status == 403)
			detail = obs_module_text("PTZ.ONVIF.Discovery.AuthFailed");
		m_step = Step::Idle;
		emit error(detail);
		return;
	}

	QByteArray rawPayload = reply->readAll();
	QByteArray payload = sanitizeXmlAmpersands(rawPayload);
	QDomDocument doc;
	QString xmlErr;
	int xmlLine = 0, xmlCol = 0;
#if QT_VERSION >= QT_VERSION_CHECK(6, 8, 0)
	QDomDocument::ParseOptions options = QDomDocument::ParseOption::UseNamespaceProcessing;
	auto pr = doc.setContent(payload, options);
	bool parsed = (bool)pr;
	if (!parsed) {
		xmlErr = pr.errorMessage;
		xmlLine = pr.errorLine;
		xmlCol = pr.errorColumn;
	}
#else
	bool parsed = doc.setContent(payload, true, &xmlErr, &xmlLine, &xmlCol);
#endif
	if (!parsed) {
		m_step = Step::Idle;
		QFile dump("/tmp/onvif-probe-bad-response.xml");
		if (dump.open(QIODevice::WriteOnly | QIODevice::Truncate))
			dump.write(rawPayload);
		emit error(QString("SOAP parse error at line %1 col %2: %3 "
				   "(body saved to /tmp/onvif-probe-bad-response.xml)")
				   .arg(xmlLine)
				   .arg(xmlCol)
				   .arg(xmlErr));
		return;
	}
	/* Surface SOAP faults — when credentials are wrong or the camera
	 * rejects our envelope, it replies with a fault rather than the
	 * expected operation response. Without this the user just sees
	 * "no Media service" and has no idea auth failed. */
	const QString nsSoap = "http://www.w3.org/2003/05/soap-envelope";
	auto faults = doc.elementsByTagNameNS(nsSoap, "Fault");
	if (!faults.isEmpty()) {
		QString reason;
		auto reasonEl = faults.at(0).toElement().elementsByTagNameNS(nsSoap, "Text");
		if (!reasonEl.isEmpty())
			reason = reasonEl.at(0).toElement().text().trimmed();
		if (reason.isEmpty())
			reason = "(no Reason text)";
		m_step = Step::Idle;
		emit error(QString("Camera SOAP fault: %1").arg(reason));
		return;
	}

	switch (m_step) {
	case Step::SystemDateAndTime: {
		auto sdNodes = doc.elementsByTagNameNS(kNsOnvifDevice, "GetSystemDateAndTimeResponse");
		if (!sdNodes.isEmpty()) {
			auto sysEl = sdNodes.at(0).toElement().firstChildElement("SystemDateAndTime", kNsOnvifDevice);
			auto utcEl = sysEl.firstChildElement("UTCDateTime", kNsOnvifSchema);
			if (!utcEl.isNull()) {
				auto dateEl = utcEl.firstChildElement("Date", kNsOnvifSchema);
				auto timeEl = utcEl.firstChildElement("Time", kNsOnvifSchema);
				int y = dateEl.firstChildElement("Year", kNsOnvifSchema).text().toInt();
				int mo = dateEl.firstChildElement("Month", kNsOnvifSchema).text().toInt();
				int d = dateEl.firstChildElement("Day", kNsOnvifSchema).text().toInt();
				int h = timeEl.firstChildElement("Hour", kNsOnvifSchema).text().toInt();
				int mi = timeEl.firstChildElement("Minute", kNsOnvifSchema).text().toInt();
				int se = timeEl.firstChildElement("Second", kNsOnvifSchema).text().toInt();
				/* See PTZOnvif::handleGetSystemDateAndTimeResponse — the
				 * (date, time, Qt::TimeSpec) constructor is deprecated
				 * in Qt 6.5. */
				QDateTime cameraTime(QDate(y, mo, d), QTime(h, mi, se));
				cameraTime.setTimeZone(QTimeZone::utc());
				if (cameraTime.isValid())
					m_timeOffsetSecs = QDateTime::currentDateTimeUtc().secsTo(cameraTime);
			}
		}
		m_step = Step::Capabilities;
		requestCapabilities();
		break;
	}
	case Step::Capabilities: {
		auto mediaNodes = doc.elementsByTagNameNS(kNsOnvifSchema, "Media");
		if (mediaNodes.isEmpty()) {
			m_step = Step::Idle;
			emit error("Camera capabilities response had no Media service");
			return;
		}
		auto mediaEl = mediaNodes.at(0).toElement();
		auto xaddr = mediaEl.firstChildElement("XAddr", kNsOnvifSchema);
		if (xaddr.isNull()) {
			m_step = Step::Idle;
			emit error("Media service has no XAddr");
			return;
		}
		QString reported = xaddr.text().trimmed();
		/* Cameras (notably Xiongmai/hsoap) often advertise their
		 * static/DHCP IP for the media service even when reached via
		 * a different IP. If the host differs from the one that's
		 * actually responding (m_host), rewrite it. */
		QUrl mediaUrl(reported);
		if (mediaUrl.isValid() && mediaUrl.host() != m_host && !m_host.isEmpty()) {
			mediaUrl.setHost(m_host);
			m_mediaXAddr = mediaUrl.toString();
		} else {
			m_mediaXAddr = reported;
		}
		m_step = Step::Profiles;
		requestProfiles();
		break;
	}
	case Step::Profiles: {
		auto profileNodes = doc.elementsByTagNameNS(kNsOnvifMedia, "Profiles");
		for (int i = 0; i < profileNodes.length(); i++) {
			auto el = profileNodes.at(i).toElement();
			QString token = el.attribute("token");
			QString name;
			auto nameEl = el.firstChildElement("Name", kNsOnvifSchema);
			if (!nameEl.isNull())
				name = nameEl.text().trimmed();
			if (!token.isEmpty()) {
				m_profileTokens.append(token);
				m_profileNames.append(name);
			}
		}
		if (m_profileTokens.isEmpty()) {
			m_step = Step::Idle;
			emit error("Camera returned no media profiles");
			return;
		}
		m_profileIdx = 0;
		m_step = Step::StreamUri;
		requestNextStreamUri();
		break;
	}
	case Step::StreamUri: {
		auto uriNodes = doc.elementsByTagNameNS(kNsOnvifSchema, "Uri");
		QString uri;
		if (uriNodes.length() > 0)
			uri = uriNodes.at(0).toElement().text().trimmed();
		StreamInfo s;
		s.profileToken = m_profileTokens.at(m_profileIdx);
		s.profileName = m_profileNames.at(m_profileIdx);
		s.uri = uri;
		m_results.append(s);
		m_profileIdx++;
		requestNextStreamUri();
		break;
	}
	case Step::Idle:
		break;
	}
}

/* ------------------------------------------------------------------------ */
/* OnvifDiscoveryDialog                                                     */
/* ------------------------------------------------------------------------ */

OnvifDiscoveryDialog::OnvifDiscoveryDialog(QWidget *parent) : QDialog(parent)
{
	setWindowTitle(obs_module_text("PTZ.ONVIF.Discovery.Title"));
	resize(760, 480);

	m_discovery = new OnvifDiscovery(this);
	connect(m_discovery, &OnvifDiscovery::cameraFound, this, &OnvifDiscoveryDialog::onCameraFound);
	connect(m_discovery, &OnvifDiscovery::finished, this, &OnvifDiscoveryDialog::onDiscoveryFinished);

	m_mediaProbe = new OnvifMediaProbe(this);
	connect(m_mediaProbe, &OnvifMediaProbe::streamsReady, this, &OnvifDiscoveryDialog::onStreamsReady);
	connect(m_mediaProbe, &OnvifMediaProbe::error, this, &OnvifDiscoveryDialog::onStreamProbeError);

	auto *layout = new QVBoxLayout(this);

	auto *topRow = new QHBoxLayout();
	m_statusLabel = new QLabel(obs_module_text("PTZ.ONVIF.Discovery.Searching"));
	m_rescanButton = new QPushButton(obs_module_text("PTZ.ONVIF.Discovery.Rescan"));
	connect(m_rescanButton, &QPushButton::clicked, this, &OnvifDiscoveryDialog::onRescanClicked);
	topRow->addWidget(m_statusLabel, 1);
	topRow->addWidget(m_rescanButton);
	layout->addLayout(topRow);

	auto *credForm = new QFormLayout();
	credForm->setLabelAlignment(Qt::AlignRight);
	m_userEdit = new QLineEdit(this);
	m_userEdit->setText("admin");
	m_passEdit = new QLineEdit(this);
	m_passEdit->setEchoMode(QLineEdit::Password);
	m_passEdit->setText("admin");
	credForm->addRow(obs_module_text("PTZ.Device.Username"), m_userEdit);
	credForm->addRow(obs_module_text("PTZ.Device.Password"), m_passEdit);
	auto *credBox = new QWidget(this);
	credBox->setLayout(credForm);
	auto *credLabel = new QLabel(obs_module_text("PTZ.ONVIF.Discovery.CredentialsHint"), this);
	credLabel->setWordWrap(true);
	layout->addWidget(credLabel);
	layout->addWidget(credBox);
	connect(m_userEdit, &QLineEdit::editingFinished, this, &OnvifDiscoveryDialog::onCredentialsEdited);
	connect(m_passEdit, &QLineEdit::editingFinished, this, &OnvifDiscoveryDialog::onCredentialsEdited);

	auto *splitter = new QSplitter(Qt::Horizontal, this);

	m_table = new QTableWidget(0, 4, this);
	QStringList headers{obs_module_text("PTZ.ONVIF.Discovery.Col.Host"),
			    obs_module_text("PTZ.ONVIF.Discovery.Col.Port"),
			    obs_module_text("PTZ.ONVIF.Discovery.Col.Manufacturer"),
			    obs_module_text("PTZ.ONVIF.Discovery.Col.Model")};
	m_table->setHorizontalHeaderLabels(headers);
	m_table->setSelectionBehavior(QAbstractItemView::SelectRows);
	m_table->setSelectionMode(QAbstractItemView::SingleSelection);
	m_table->setEditTriggers(QAbstractItemView::NoEditTriggers);
	m_table->verticalHeader()->setVisible(false);
	m_table->horizontalHeader()->setStretchLastSection(true);
	connect(m_table, &QTableWidget::itemSelectionChanged, this, &OnvifDiscoveryDialog::onSelectionChanged);
	connect(m_table, &QTableWidget::itemDoubleClicked, this, [this](QTableWidgetItem *) { accept(); });
	splitter->addWidget(m_table);

	m_detailView = new QPlainTextEdit(this);
	m_detailView->setReadOnly(true);
	m_detailView->setPlaceholderText(obs_module_text("PTZ.ONVIF.Discovery.SelectHint"));
	splitter->addWidget(m_detailView);
	splitter->setStretchFactor(0, 3);
	splitter->setStretchFactor(1, 2);
	layout->addWidget(splitter, 1);

	auto *manualRow = new QHBoxLayout();
	auto *manualLabel = new QLabel(obs_module_text("PTZ.ONVIF.Discovery.ManualPrompt"), this);
	m_manualHostEdit = new QLineEdit(this);
	m_manualHostEdit->setPlaceholderText("192.168.1.100");
	m_manualPortEdit = new QLineEdit(this);
	m_manualPortEdit->setPlaceholderText("80");
	m_manualPortEdit->setFixedWidth(60);
	m_addManualButton = new QPushButton(obs_module_text("PTZ.ONVIF.Discovery.ManualAdd"), this);
	manualRow->addWidget(manualLabel);
	manualRow->addWidget(m_manualHostEdit, 1);
	manualRow->addWidget(new QLabel(":", this));
	manualRow->addWidget(m_manualPortEdit);
	manualRow->addWidget(m_addManualButton);
	layout->addLayout(manualRow);
	connect(m_addManualButton, &QPushButton::clicked, this, &OnvifDiscoveryDialog::onAddManualClicked);
	connect(m_manualHostEdit, &QLineEdit::returnPressed, this, &OnvifDiscoveryDialog::onAddManualClicked);
	connect(m_manualPortEdit, &QLineEdit::returnPressed, this, &OnvifDiscoveryDialog::onAddManualClicked);

	auto *mediaSourceRow = new QHBoxLayout();
	m_addMediaSourceCheck = new QCheckBox(obs_module_text("PTZ.ONVIF.Discovery.CreateMediaSource"), this);
	m_addMediaSourceCheck->setChecked(true);
	m_streamCombo = new QComboBox(this);
	m_streamCombo->setEnabled(false);
	m_streamCombo->setMinimumContentsLength(20);
	mediaSourceRow->addWidget(m_addMediaSourceCheck);
	mediaSourceRow->addWidget(m_streamCombo, 1);
	layout->addLayout(mediaSourceRow);

	auto *buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this);
	m_okButton = buttons->button(QDialogButtonBox::Ok);
	m_okButton->setText(obs_module_text("PTZ.ONVIF.Discovery.UseCamera"));
	m_okButton->setEnabled(false);
	connect(buttons, &QDialogButtonBox::accepted, this, &OnvifDiscoveryDialog::accept);
	connect(buttons, &QDialogButtonBox::rejected, this, &OnvifDiscoveryDialog::reject);
	layout->addWidget(buttons);

	/* Auto-start the first scan as soon as the dialog appears. */
	QTimer::singleShot(0, this, [this]() { onRescanClicked(); });
}

void OnvifDiscoveryDialog::onRescanClicked()
{
	if (m_discovery->isRunning())
		return;
	m_cameras.clear();
	m_table->setRowCount(0);
	m_okButton->setEnabled(false);
	m_rescanButton->setEnabled(false);
	m_statusLabel->setText(obs_module_text("PTZ.ONVIF.Discovery.Searching"));
	m_detailView->clear();
	m_discovery->start(4000);
}

void OnvifDiscoveryDialog::appendCamera(const OnvifCameraInfo &cam)
{
	m_cameras.append(cam);
	int row = m_table->rowCount();
	m_table->insertRow(row);
	auto *hostItem = new QTableWidgetItem(cam.host);
	hostItem->setData(Qt::UserRole, m_cameras.size() - 1);
	m_table->setItem(row, 0, hostItem);
	m_table->setItem(row, 1, new QTableWidgetItem(QString::number(cam.port)));
	m_table->setItem(row, 2, new QTableWidgetItem(cam.manufacturer));
	m_table->setItem(row, 3, new QTableWidgetItem(cam.model));
	m_table->resizeColumnsToContents();
}

void OnvifDiscoveryDialog::onCameraFound(const OnvifCameraInfo &cam)
{
	appendCamera(cam);
	if (m_table->rowCount() == 1)
		m_table->selectRow(0);
}

void OnvifDiscoveryDialog::onAddManualClicked()
{
	QString host = m_manualHostEdit->text().trimmed();
	if (host.isEmpty())
		return;
	bool portOk = false;
	int port = m_manualPortEdit->text().trimmed().toInt(&portOk);
	if (!portOk || port <= 0 || port > 65535)
		port = 80;

	OnvifCameraInfo cam;
	cam.host = host;
	cam.port = port;
	cam.xaddr = QString("http://%1:%2/onvif/device_service").arg(host).arg(port);
	cam.manufacturer = obs_module_text("PTZ.ONVIF.Discovery.ManualLabel");
	cam.name = host;
	appendCamera(cam);
	m_table->selectRow(m_table->rowCount() - 1);
	m_manualHostEdit->clear();
}

void OnvifDiscoveryDialog::onDiscoveryFinished()
{
	m_rescanButton->setEnabled(true);
	QString msg;
	if (m_cameras.isEmpty()) {
		msg = obs_module_text("PTZ.ONVIF.Discovery.NoResponse");
	} else {
		msg = QString(obs_module_text("PTZ.ONVIF.Discovery.Found")).arg(m_cameras.size());
	}
	m_statusLabel->setText(msg);
}

void OnvifDiscoveryDialog::onSelectionChanged()
{
	auto items = m_table->selectedItems();
	if (items.isEmpty()) {
		m_okButton->setEnabled(false);
		m_selected = OnvifCameraInfo();
		m_streams.clear();
		m_streamStatus.clear();
		m_streamCombo->clear();
		m_streamCombo->setEnabled(false);
		m_detailView->clear();
		return;
	}
	int idx = items.first()->data(Qt::UserRole).toInt();
	if (idx < 0 || idx >= m_cameras.size())
		return;
	m_selected = m_cameras.at(idx);
	m_okButton->setEnabled(true);
	m_streams.clear();
	m_streamStatus.clear();
	m_streamCombo->clear();
	m_streamCombo->setEnabled(false);
	rebuildDetail();
	maybeProbeStreams();
}

void OnvifDiscoveryDialog::onCredentialsEdited()
{
	maybeProbeStreams();
}

void OnvifDiscoveryDialog::maybeProbeStreams()
{
	if (m_selected.xaddr.isEmpty())
		return;
	QString user = m_userEdit->text();
	QString pass = m_passEdit->text();
	if (user.isEmpty() && pass.isEmpty()) {
		m_streamStatus = obs_module_text("PTZ.ONVIF.Discovery.NeedCreds");
		m_streams.clear();
		rebuildDetail();
		return;
	}
	m_streamStatus = obs_module_text("PTZ.ONVIF.Discovery.FetchingStreams");
	m_streams.clear();
	rebuildDetail();
	m_mediaProbe->fetch(m_selected.host, m_selected.port, user, pass);
}

void OnvifDiscoveryDialog::onStreamsReady(const QList<OnvifMediaProbe::StreamInfo> &streams)
{
	m_streams = streams;
	m_streamStatus.clear();
	rebuildDetail();
	/* Re-populate the stream picker so the user can choose Main vs Sub
	 * for the auto-created Media Source. Falls back to disabled when no
	 * streams are available. */
	m_streamCombo->clear();
	for (const auto &s : m_streams) {
		QString label = s.profileName.isEmpty() ? s.profileToken : s.profileName;
		m_streamCombo->addItem(label, s.uri);
	}
	m_streamCombo->setEnabled(m_streamCombo->count() > 0);
}

void OnvifDiscoveryDialog::onStreamProbeError(const QString &msg)
{
	m_streams.clear();
	m_streamStatus = QString(obs_module_text("PTZ.ONVIF.Discovery.StreamError")).arg(msg);
	rebuildDetail();
}

void OnvifDiscoveryDialog::rebuildDetail()
{
	QStringList lines;
	lines << QString("Host: %1").arg(m_selected.host);
	lines << QString("Port: %1").arg(m_selected.port);
	if (!m_selected.manufacturer.isEmpty())
		lines << QString("Manufacturer: %1").arg(m_selected.manufacturer);
	if (!m_selected.model.isEmpty())
		lines << QString("Model: %1").arg(m_selected.model);
	if (!m_selected.location.isEmpty())
		lines << QString("Location: %1").arg(m_selected.location);
	if (!m_selected.uuid.isEmpty())
		lines << QString("UUID: %1").arg(m_selected.uuid);
	lines << QString("Device service: %1").arg(m_selected.xaddr);

	lines << "";
	lines << QString("%1:").arg(obs_module_text("PTZ.ONVIF.Discovery.Streams"));
	if (!m_streams.isEmpty()) {
		for (const auto &s : m_streams) {
			QString label = s.profileName.isEmpty() ? s.profileToken : s.profileName;
			lines << QString("  [%1] %2").arg(label, s.uri);
		}
	} else if (!m_streamStatus.isEmpty()) {
		lines << "  " + m_streamStatus;
	}

	if (!m_selected.types.isEmpty()) {
		lines << "";
		lines << "Types:";
		for (const auto &t : m_selected.types)
			lines << "  " + t;
	}
	if (!m_selected.scopes.isEmpty()) {
		lines << "";
		lines << "Scopes:";
		for (const auto &s : m_selected.scopes)
			lines << "  " + s;
	}
	m_detailView->setPlainText(lines.join("\n"));
}

void OnvifDiscoveryDialog::accept()
{
	if (m_selected.xaddr.isEmpty()) {
		QDialog::reject();
		return;
	}
	if (m_discovery->isRunning())
		m_discovery->stop();
	m_mediaProbe->cancel();
	QDialog::accept();
}

QString OnvifDiscoveryDialog::selectedUsername() const
{
	return m_userEdit ? m_userEdit->text() : QString();
}

QString OnvifDiscoveryDialog::selectedPassword() const
{
	return m_passEdit ? m_passEdit->text() : QString();
}

QString OnvifDiscoveryDialog::selectedStreamUri() const
{
	if (m_streamCombo && m_streamCombo->currentIndex() >= 0) {
		QString uri = m_streamCombo->currentData().toString();
		if (!uri.isEmpty())
			return uri;
	}
	for (const auto &s : m_streams) {
		if (!s.uri.isEmpty())
			return s.uri;
	}
	return QString();
}

bool OnvifDiscoveryDialog::createMediaSourceRequested() const
{
	return m_addMediaSourceCheck && m_addMediaSourceCheck->isChecked();
}
