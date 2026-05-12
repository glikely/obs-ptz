/* Pan Tilt Zoom ONVIF implementation
 *
 * Copyright 2022 Jonatã Bolzan Loss <jonata@jonata.org>
 *
 * SPDX-License-Identifier: GPLv2
 *
 * ONVIF support is experimental and needs rework to be non-blocking before it
 * can be enabled generically. This code is disabled by default. To use it
 * requires manually adding an ONVIF entry to the config file.
 */

#include <qt-wrappers.hpp>
#include "ptz-onvif.hpp"
#include <QtXml/QDomDocument>
#include <QXmlStreamWriter>
#include <QRegularExpression>
#include <QTimeZone>

void PTZOnvif::sendRequest(QString url, QString req)
{
	if (url.isEmpty()) {
		/* Happens when an operation fires before GetCapabilities has
		 * populated the per-service XAddrs (e.g. the camera was added
		 * with an unreachable host). Quietly drop instead of letting
		 * Qt spam "Protocol \"\" is unknown" once per command. */
		return;
	}
	QNetworkRequest request(url);
	request.setHeader(QNetworkRequest::ContentTypeHeader, "application/soap+xml; charset=utf-8");
	/* Surface failures in seconds instead of waiting on the OS TCP
	 * timeout (~2 min). 10s is generous for an ONVIF SOAP exchange. */
	request.setTransferTimeout(10000);
	/* Authentication is carried in the SOAP envelope via WS-Security
	 * UsernameToken (see writeHeader). If a camera actually demands HTTP
	 * Digest, authRequired() supplies it on demand. Sending a Basic
	 * Authorization header unconditionally caused some firmwares to
	 * reject the request as ambiguous. */
	m_networkManager.post(request, req.toUtf8());
}

void PTZOnvif::authRequired(QNetworkReply *, QAuthenticator *authenticator)
{
	authenticator->setUser(username);
	authenticator->setPassword(password);
}

const QString nsXmlSchema("http://www.w3.org/2001/XMLSchema");                  //xsd
const QString nsXmlSchemaInstance("http://www.w3.org/2001/XMLSchema-instance"); //xsi
const QString nsSoapEnvelope("http://www.w3.org/2003/05/soap-envelope");        //SOAP-ENV
const QString nsSoapEncoding("http://www.w3.org/2003/05/soap-encoding");        //SOAP-ENC
const QString nsAddressing("http://www.w3.org/2005/08/addressing");             //wsa5
const QString nsOnvifSchema("http://www.onvif.org/ver10/schema");               //tt
const QString nsOnvifDevice("http://www.onvif.org/ver10/device/wsdl");          //tds
const QString nsOnvifMedia("http://www.onvif.org/ver10/media/wsdl");            //trt
const QString nsOnvifPtz("http://www.onvif.org/ver20/ptz/wsdl");                //tptz
const QString nsWssSecext("http://docs.oasis-open.org/wss/2004/01/oasis-200401-wss-wssecurity-secext-1.0.xsd");   //wsse
const QString nsWssUtility("http://docs.oasis-open.org/wss/2004/01/oasis-200401-wss-wssecurity-utility-1.0.xsd"); //wsu
const QString nsWssPasswordDigest(
	"http://docs.oasis-open.org/wss/2004/01/oasis-200401-wss-username-token-profile-1.0#PasswordDigest");

static void writeStartOnvifDocument(QXmlStreamWriter &s)
{
	s.setAutoFormatting(true);
	s.setAutoFormattingIndent(1);
	s.writeStartDocument();
	s.writeNamespace(nsSoapEnvelope, "SOAP-ENV");
	s.writeNamespace(nsSoapEncoding, "SOAP-ENC");
	s.writeNamespace(nsAddressing, "wsa5");
	s.writeNamespace(nsXmlSchemaInstance, "xsi");
	s.writeNamespace(nsXmlSchema, "xsd");
	s.writeNamespace(nsOnvifSchema, "tt");
	s.writeNamespace(nsOnvifDevice, "tds");
	s.writeNamespace(nsOnvifMedia, "trt");
	s.writeNamespace(nsOnvifPtz, "tptz");
	s.writeNamespace(nsWssSecext, "wsse");
	s.writeNamespace(nsWssUtility, "wsu");
}

void PTZOnvif::writeHeader(QXmlStreamWriter &s, const QString action = "")
{
	QUuid nonce = QUuid::createUuid();
	QString nonce64 = nonce.toByteArray().toBase64();
	/* Adjust by the per-camera clock offset so cameras with bad NTP
	 * don't reject our WS-Security timestamp. */
	auto timestamp = QDateTime::currentDateTimeUtc().addSecs(m_timeOffsetSecs).toString(Qt::ISODate);
	auto token = nonce.toString() + timestamp + password;
	QCryptographicHash hash(QCryptographicHash::Sha1);
	hash.addData(token.toUtf8());
	QString hashTokenBase64 = hash.result().toBase64();

	s.writeStartElement(nsSoapEnvelope, "Header");
	if (action != "") {
		s.writeStartElement(nsAddressing, "Action");
		s.writeAttribute(nsSoapEnvelope, "mustUnderstand", "1");
		s.writeCharacters(action);
		s.writeEndElement(); // Action
	}
	s.writeStartElement(nsWssSecext, "Security");
	s.writeAttribute(nsSoapEnvelope, "mustUnderstand", "1");
	s.writeStartElement(nsWssSecext, "UsernameToken");
	s.writeTextElement(nsWssSecext, "Username", username);
	s.writeStartElement(nsWssSecext, "Password");
	s.writeAttribute("Type", nsWssPasswordDigest);
	s.writeCharacters(hashTokenBase64);
	s.writeEndElement(); // Password
	s.writeStartElement(nsWssSecext, "Nonce");
	s.writeAttribute("EncodingType", nsWssPasswordDigest);
	s.writeCharacters(nonce64);
	s.writeEndElement(); // Nonce
	s.writeTextElement(nsWssUtility, "Created", timestamp);
	s.writeEndElement(); // UsernameToken
	s.writeEndElement(); // Security
	s.writeEndElement(); // Header
}

static void writePanTilt(QXmlStreamWriter &s, double pan, double tilt)
{
	s.writeEmptyElement(nsOnvifSchema, "PanTilt");
	s.writeAttribute("x", QString::number(pan));
	s.writeAttribute("y", QString::number(tilt));
}

static void writeZoom(QXmlStreamWriter &s, double zoom)
{
	s.writeEmptyElement(nsOnvifSchema, "Zoom");
	s.writeAttribute("x", QString::number(zoom));
}

void PTZOnvif::genericMove(QString movetype, QString property, double x, double y, double z)
{
	QString msg;
	QXmlStreamWriter s(&msg);
	writeStartOnvifDocument(s);
	s.writeStartElement(nsSoapEnvelope, "Envelope");
	writeHeader(s, nsOnvifPtz + "/" + movetype);
	s.writeStartElement(nsSoapEnvelope, "Body");
	s.writeStartElement(nsOnvifPtz, movetype);
	s.writeTextElement(nsOnvifPtz, "ProfileToken", m_selectedMedia.token);
	s.writeStartElement(nsOnvifPtz, property);
	writePanTilt(s, x, y);
	writeZoom(s, z);
	s.writeEndElement(); // property
	s.writeEndElement(); // movetype
	s.writeEndElement(); // Body
	s.writeEndElement(); // Envelope
	s.writeEndDocument();
	sendRequest(m_PTZAddress, msg);
}

void PTZOnvif::continuousMove(double x, double y, double z)
{
	genericMove("ContinuousMove", "Velocity", x, y, z);
}

void PTZOnvif::absoluteMove(double x, double y, double z)
{
	genericMove("AbsoluteMove", "Position", x, y, z);
}

void PTZOnvif::relativeMove(double x, double y, double z)
{
	genericMove("RelativeMove", "Translation", x, y, z);
}

void PTZOnvif::stop()
{
	QString msg;
	QXmlStreamWriter s(&msg);
	writeStartOnvifDocument(s);
	s.writeStartElement(nsSoapEnvelope, "Envelope");
	writeHeader(s, nsOnvifPtz + "/" + "Stop");
	s.writeStartElement(nsSoapEnvelope, "Body");
	s.writeStartElement(nsOnvifPtz, "Stop");
	s.writeTextElement(nsOnvifPtz, "ProfileToken", m_selectedMedia.token);
	s.writeTextElement(nsOnvifPtz, "PanTilt", "true");
	s.writeTextElement(nsOnvifPtz, "Zoom", "true");
	s.writeEndElement(); // movetype
	s.writeEndElement(); // Body
	s.writeEndElement(); // Envelope
	s.writeEndDocument();
	sendRequest(m_PTZAddress, msg);
}

void PTZOnvif::goToHomePosition()
{
	QString msg;
	QXmlStreamWriter s(&msg);
	writeStartOnvifDocument(s);
	s.writeStartElement(nsSoapEnvelope, "Envelope");
	writeHeader(s, nsOnvifPtz + "/" + "GotoHomePosition");
	s.writeStartElement(nsSoapEnvelope, "Body");
	s.writeStartElement(nsOnvifPtz, "GotoHomePosition");
	s.writeTextElement(nsOnvifPtz, "ProfileToken", m_selectedMedia.token);
	s.writeEndElement(); // GotoHomePosition
	s.writeEndElement(); // Body
	s.writeEndElement(); // Envelope
	s.writeEndDocument();
	sendRequest(m_PTZAddress, msg);
}

void PTZOnvif::memory_set(int i)
{
	QString token = m_presetsModel.presetProperty(i, "token").toString();
	QString name = m_presetsModel.presetProperty(i, "name").toString();
	/* Remember which slot the response should be filed under, so we can
	 * link the camera-assigned PresetToken back to the right local slot. */
	m_pendingSetPresetSlot = i;
	QString msg;
	QXmlStreamWriter s(&msg);
	writeStartOnvifDocument(s);
	s.writeStartElement(nsSoapEnvelope, "Envelope");
	writeHeader(s, nsOnvifPtz + "/" + "SetPreset");
	s.writeStartElement(nsSoapEnvelope, "Body");
	s.writeStartElement(nsOnvifPtz, "SetPreset");
	s.writeTextElement(nsOnvifPtz, "ProfileToken", m_selectedMedia.token);
	if (name != "")
		s.writeTextElement(nsOnvifPtz, "PresetName", name);
	if (token != "")
		s.writeTextElement(nsOnvifPtz, "PresetToken", token);
	s.writeEndElement(); // movetype
	s.writeEndElement(); // Body
	s.writeEndElement(); // Envelope
	s.writeEndDocument();
	sendRequest(m_PTZAddress, msg);
}

void PTZOnvif::memory_reset(int i)
{
	QString token = m_presetsModel.presetProperty(i, "token").toString();
	if (token == "")
		return;
	QString msg;
	QXmlStreamWriter s(&msg);
	writeStartOnvifDocument(s);
	s.writeStartElement(nsSoapEnvelope, "Envelope");
	writeHeader(s, nsOnvifPtz + "/" + "RemovePreset");
	s.writeStartElement(nsSoapEnvelope, "Body");
	s.writeStartElement(nsOnvifPtz, "RemovePreset");
	s.writeTextElement(nsOnvifPtz, "ProfileToken", m_selectedMedia.token);
	s.writeTextElement(nsOnvifPtz, "PresetToken", token);
	s.writeEndElement(); // movetype
	s.writeEndElement(); // Body
	s.writeEndElement(); // Envelope
	s.writeEndDocument();
	sendRequest(m_PTZAddress, msg);
	/* The preset is gone from the camera; clear the stale token locally so
	 * a future memory_set on this slot creates a new one instead of trying
	 * to update a token the camera doesn't know about. */
	QVariantMap clear;
	clear["token"] = QString();
	m_presetsModel.updatePreset(i, clear);
}

void PTZOnvif::memory_recall(int i)
{
	QString token = m_presetsModel.presetProperty(i, "token").toString();
	if (token == "")
		return;
	QString msg;
	QXmlStreamWriter s(&msg);
	writeStartOnvifDocument(s);
	s.writeStartElement(nsSoapEnvelope, "Envelope");
	writeHeader(s, nsOnvifPtz + "/" + "GotoPreset");
	s.writeStartElement(nsSoapEnvelope, "Body");
	s.writeStartElement(nsOnvifPtz, "GotoPreset");
	s.writeTextElement(nsOnvifPtz, "ProfileToken", m_selectedMedia.token);
	s.writeTextElement(nsOnvifPtz, "PresetToken", token);
	s.writeEndElement(); // movetype
	s.writeEndElement(); // Body
	s.writeEndElement(); // Envelope
	s.writeEndDocument();
	sendRequest(m_PTZAddress, msg);
}

void PTZOnvif::getPresets()
{
	QString msg;
	QXmlStreamWriter s(&msg);
	writeStartOnvifDocument(s);
	s.writeStartElement(nsSoapEnvelope, "Envelope");
	writeHeader(s, nsOnvifPtz + "/" + "GetPresets");
	s.writeStartElement(nsSoapEnvelope, "Body");
	s.writeStartElement(nsOnvifPtz, "GetPresets");
	s.writeTextElement(nsOnvifPtz, "ProfileToken", m_selectedMedia.token);
	s.writeEndElement(); // movetype
	s.writeEndElement(); // Body
	s.writeEndElement(); // Envelope
	s.writeEndDocument();
	sendRequest(m_PTZAddress, msg);
}

void PTZOnvif::getStatus()
{
	if (m_PTZAddress.isEmpty() || m_selectedMedia.token.isEmpty())
		return;
	QString msg;
	QXmlStreamWriter s(&msg);
	writeStartOnvifDocument(s);
	s.writeStartElement(nsSoapEnvelope, "Envelope");
	writeHeader(s, nsOnvifPtz + "/" + "GetStatus");
	s.writeStartElement(nsSoapEnvelope, "Body");
	s.writeStartElement(nsOnvifPtz, "GetStatus");
	s.writeTextElement(nsOnvifPtz, "ProfileToken", m_selectedMedia.token);
	s.writeEndElement(); // GetStatus
	s.writeEndElement(); // Body
	s.writeEndElement(); // Envelope
	s.writeEndDocument();
	sendRequest(m_PTZAddress, msg);
}

void PTZOnvif::handleResponse(QString response)
{
	/* Some firmwares emit unescaped '&' in URI text content (notably in
	 * GetStreamUri responses with `&channel=`/`&protocol=`). That's
	 * invalid XML and Qt's QDomDocument rejects the whole document.
	 * Replace any bare '&' not starting a known entity with '&amp;'. */
	static const QRegularExpression ampFix(QStringLiteral("&(?!(?:amp|lt|gt|quot|apos|#[0-9]+|#x[0-9a-fA-F]+);)"));
	response.replace(ampFix, "&amp;");

	QDomDocument doc;
	QDomNodeList nl;

#if QT_VERSION >= QT_VERSION_CHECK(6, 8, 0)
	QDomDocument::ParseOptions options = QDomDocument::ParseOption::UseNamespaceProcessing;
	doc.setContent(QAnyStringView(response), options);
#else
	doc.setContent(response, true);
#endif

	nl = doc.elementsByTagNameNS(nsOnvifDevice, "GetSystemDateAndTimeResponse");
	for (int i = 0; i < nl.length(); i++)
		handleGetSystemDateAndTimeResponse(nl.at(i));
	nl = doc.elementsByTagNameNS(nsOnvifDevice, "GetCapabilitiesResponse");
	for (int i = 0; i < nl.length(); i++)
		handleGetCapabilitiesResponse(nl.at(i));
	nl = doc.elementsByTagNameNS(nsOnvifMedia, "GetProfilesResponse");
	for (int i = 0; i < nl.length(); i++)
		handleGetProfilesResponse(nl.at(i));
	handleGetPresetsResponse(doc);
	handleSetPresetResponse(doc);
	nl = doc.elementsByTagNameNS(nsOnvifPtz, "GetStatusResponse");
	for (int i = 0; i < nl.length(); i++)
		handleGetStatusResponse(nl.at(i));
}

void PTZOnvif::handleGetStatusResponse(QDomNode node)
{
	auto statusEl = node.toElement().firstChildElement("PTZStatus", nsOnvifPtz);
	if (statusEl.isNull())
		return;
	auto posEl = statusEl.firstChildElement("Position", nsOnvifSchema);
	if (posEl.isNull())
		return;
	auto ptEl = posEl.firstChildElement("PanTilt", nsOnvifSchema);
	if (!ptEl.isNull()) {
		m_position_pan = ptEl.attribute("x").toDouble();
		m_position_tilt = ptEl.attribute("y").toDouble();
	}
	auto zoomEl = posEl.firstChildElement("Zoom", nsOnvifSchema);
	if (!zoomEl.isNull())
		m_position_zoom = zoomEl.attribute("x").toDouble();
	ptz_debug("status: pan=%.3f tilt=%.3f zoom=%.3f", m_position_pan, m_position_tilt, m_position_zoom);
}

void PTZOnvif::handleGetSystemDateAndTimeResponse(QDomNode node)
{
	auto sysEl = node.toElement().firstChildElement("SystemDateAndTime", nsOnvifDevice);
	auto utcEl = sysEl.firstChildElement("UTCDateTime", nsOnvifSchema);
	if (!utcEl.isNull()) {
		auto dateEl = utcEl.firstChildElement("Date", nsOnvifSchema);
		auto timeEl = utcEl.firstChildElement("Time", nsOnvifSchema);
		int y = dateEl.firstChildElement("Year", nsOnvifSchema).text().toInt();
		int mo = dateEl.firstChildElement("Month", nsOnvifSchema).text().toInt();
		int d = dateEl.firstChildElement("Day", nsOnvifSchema).text().toInt();
		int h = timeEl.firstChildElement("Hour", nsOnvifSchema).text().toInt();
		int mi = timeEl.firstChildElement("Minute", nsOnvifSchema).text().toInt();
		int se = timeEl.firstChildElement("Second", nsOnvifSchema).text().toInt();
		/* Construct as local-naive then re-anchor to UTC. The
		 * (QDate, QTime, Qt::TimeSpec) constructor was deprecated in
		 * Qt 6.5 (MSVC -Werror flags it); setTimeZone(QTimeZone::utc())
		 * does the same thing and is portable back to Qt 5.2. */
		QDateTime cameraTime(QDate(y, mo, d), QTime(h, mi, se));
		cameraTime.setTimeZone(QTimeZone::utc());
		if (cameraTime.isValid()) {
			m_timeOffsetSecs = QDateTime::currentDateTimeUtc().secsTo(cameraTime);
			if (m_timeOffsetSecs > 60 || m_timeOffsetSecs < -60)
				ptz_info("camera clock offset %lld s; adjusting WS-Security timestamps",
					 (long long)m_timeOffsetSecs);
		}
	}
	ensureCapabilitiesRequested();
}

void PTZOnvif::handleSetPresetResponse(QDomDocument &doc)
{
	if (m_pendingSetPresetSlot < 0)
		return;
	auto nl = doc.elementsByTagNameNS(nsOnvifPtz, "SetPresetResponse");
	if (nl.isEmpty())
		return;
	auto tokenNodes = nl.at(0).toElement().elementsByTagNameNS(nsOnvifPtz, "PresetToken");
	if (tokenNodes.isEmpty())
		return;
	QString newToken = tokenNodes.at(0).toElement().text().trimmed();
	if (newToken.isEmpty())
		return;
	QVariantMap map;
	map["token"] = newToken;
	m_presetsModel.updatePreset(m_pendingSetPresetSlot, map);
	m_pendingSetPresetSlot = -1;
}

/* Rewrite the host portion of a camera-reported service XAddr to whatever
 * host the user configured. Some firmwares (Xiongmai/hsoap is the canonical
 * offender) advertise their static/DHCP IP for every service even when the
 * device is actually being reached through a different IP/route. Trusting
 * those values verbatim leaves us trying to connect to an unreachable host
 * once we move from the device service to media/ptz/imaging. */
static QString rewriteXAddrHost(const QString &xaddr, const QString &knownHost)
{
	if (xaddr.isEmpty() || knownHost.isEmpty())
		return xaddr;
	QUrl url(xaddr.trimmed());
	if (!url.isValid())
		return xaddr;
	if (url.host() == knownHost)
		return xaddr;
	url.setHost(knownHost);
	return url.toString();
}

void PTZOnvif::handleGetCapabilitiesResponse(QDomNode node)
{
	auto responseElement = node.toElement();
	auto pl = responseElement.elementsByTagNameNS(nsOnvifSchema, "PTZ");
	for (int i = 0; i < pl.length(); i++) {
		auto e = pl.at(i).toElement();
		m_PTZAddress = rewriteXAddrHost(e.firstChildElement("XAddr", nsOnvifSchema).text(), host);
	}

	pl = responseElement.elementsByTagNameNS(nsOnvifSchema, "Media");
	for (int i = 0; i < pl.length(); i++) {
		auto e = pl.at(i).toElement();
		m_mediaXAddr = rewriteXAddrHost(e.firstChildElement("XAddr", nsOnvifSchema).text(), host);
	}

	/* Imaging service hosts focus and white balance controls. Optional. */
	pl = responseElement.elementsByTagNameNS(nsOnvifSchema, "Imaging");
	for (int i = 0; i < pl.length(); i++) {
		auto e = pl.at(i).toElement();
		m_imagingXAddr = rewriteXAddrHost(e.firstChildElement("XAddr", nsOnvifSchema).text(), host);
	}
	getProfiles();
}

void PTZOnvif::handleGetProfilesResponse(QDomNode n)
{
	m_mediaProfiles.clear();
	auto pl = n.toElement().elementsByTagNameNS(nsOnvifMedia, "Profiles");
	for (int i = 0; i < pl.length(); i++) {
		MediaProfile pro;
		auto e = pl.at(i).toElement();
		pro.token = e.attribute("token");
		pro.name = e.firstChildElement("Name", nsOnvifSchema).text();
		/* The Imaging service is keyed by VideoSourceToken (not profile
		 * token), so grab it from the VideoSourceConfiguration. */
		auto vsCfg = e.firstChildElement("VideoSourceConfiguration", nsOnvifSchema);
		if (!vsCfg.isNull()) {
			auto src = vsCfg.firstChildElement("SourceToken", nsOnvifSchema);
			if (!src.isNull())
				pro.videoSourceToken = src.text().trimmed();
		}
		m_mediaProfiles.push_back(pro);
	}
	if (m_mediaProfiles.isEmpty()) {
		getPresets();
		return;
	}
	/* Prefer the profile token the user selected last time, if it still
	 * exists on the camera; otherwise fall back to the first one. */
	m_selectedMedia = m_mediaProfiles.first();
	if (!m_savedProfileToken.isEmpty()) {
		for (const auto &p : m_mediaProfiles) {
			if (p.token == m_savedProfileToken) {
				m_selectedMedia = p;
				break;
			}
		}
	}
	getPresets();
	applyImagingIfPending();
}

void PTZOnvif::handleGetPresetsResponse(QDomDocument &doc)
{
	QDomNodeList nodes = doc.elementsByTagNameNS(nsOnvifPtz, "Preset");
	for (int i = 0; i < nodes.length(); i++) {
		auto node = nodes.at(i).toElement();
		QString token = node.attribute("token");
		auto child = node.firstChildElement("Name", nsOnvifSchema);
		QString name = child.text();
		if (token == "")
			continue;

		QVariantMap map;
		auto psid = m_presetsModel.find("token", token);
		if (psid < 0) {
			psid = m_presetsModel.newPreset();
			map["token"] = token;
		}
		if (psid < 0)
			continue;
		if (name != "")
			map["name"] = name;
		m_presetsModel.updatePreset(psid, map);
	}
}

const QString nsOnvifImaging("http://www.onvif.org/ver20/imaging/wsdl"); //timg

void PTZOnvif::imagingFocusMove(double speed)
{
	if (m_imagingXAddr.isEmpty() || m_selectedMedia.videoSourceToken.isEmpty())
		return;
	QString msg;
	QXmlStreamWriter s(&msg);
	writeStartOnvifDocument(s);
	s.writeNamespace(nsOnvifImaging, "timg");
	s.writeStartElement(nsSoapEnvelope, "Envelope");
	writeHeader(s, nsOnvifImaging + "/Move");
	s.writeStartElement(nsSoapEnvelope, "Body");
	s.writeStartElement(nsOnvifImaging, "Move");
	s.writeTextElement(nsOnvifImaging, "VideoSourceToken", m_selectedMedia.videoSourceToken);
	s.writeStartElement(nsOnvifImaging, "Focus");
	s.writeStartElement(nsOnvifSchema, "Continuous");
	s.writeTextElement(nsOnvifSchema, "Speed", QString::number(speed));
	s.writeEndElement(); // Continuous
	s.writeEndElement(); // Focus
	s.writeEndElement(); // Move
	s.writeEndElement(); // Body
	s.writeEndElement(); // Envelope
	s.writeEndDocument();
	sendRequest(m_imagingXAddr, msg);
}

void PTZOnvif::imagingFocusStop()
{
	if (m_imagingXAddr.isEmpty() || m_selectedMedia.videoSourceToken.isEmpty())
		return;
	QString msg;
	QXmlStreamWriter s(&msg);
	writeStartOnvifDocument(s);
	s.writeNamespace(nsOnvifImaging, "timg");
	s.writeStartElement(nsSoapEnvelope, "Envelope");
	writeHeader(s, nsOnvifImaging + "/Stop");
	s.writeStartElement(nsSoapEnvelope, "Body");
	s.writeStartElement(nsOnvifImaging, "Stop");
	s.writeTextElement(nsOnvifImaging, "VideoSourceToken", m_selectedMedia.videoSourceToken);
	s.writeEndElement(); // Stop
	s.writeEndElement(); // Body
	s.writeEndElement(); // Envelope
	s.writeEndDocument();
	sendRequest(m_imagingXAddr, msg);
}

void PTZOnvif::imagingSetAutoFocus(bool autoFocus)
{
	if (m_imagingXAddr.isEmpty() || m_selectedMedia.videoSourceToken.isEmpty())
		return;
	QString msg;
	QXmlStreamWriter s(&msg);
	writeStartOnvifDocument(s);
	s.writeNamespace(nsOnvifImaging, "timg");
	s.writeStartElement(nsSoapEnvelope, "Envelope");
	writeHeader(s, nsOnvifImaging + "/SetImagingSettings");
	s.writeStartElement(nsSoapEnvelope, "Body");
	s.writeStartElement(nsOnvifImaging, "SetImagingSettings");
	s.writeTextElement(nsOnvifImaging, "VideoSourceToken", m_selectedMedia.videoSourceToken);
	s.writeStartElement(nsOnvifImaging, "ImagingSettings");
	s.writeStartElement(nsOnvifSchema, "Focus");
	s.writeTextElement(nsOnvifSchema, "AutoFocusMode", autoFocus ? "AUTO" : "MANUAL");
	s.writeEndElement(); // Focus
	s.writeEndElement(); // ImagingSettings
	s.writeEndElement(); // SetImagingSettings
	s.writeEndElement(); // Body
	s.writeEndElement(); // Envelope
	s.writeEndDocument();
	sendRequest(m_imagingXAddr, msg);
}

void PTZOnvif::imagingSetWhiteBalance(const QString &mode)
{
	if (m_imagingXAddr.isEmpty() || m_selectedMedia.videoSourceToken.isEmpty() || mode.isEmpty())
		return;
	QString msg;
	QXmlStreamWriter s(&msg);
	writeStartOnvifDocument(s);
	s.writeNamespace(nsOnvifImaging, "timg");
	s.writeStartElement(nsSoapEnvelope, "Envelope");
	writeHeader(s, nsOnvifImaging + "/SetImagingSettings");
	s.writeStartElement(nsSoapEnvelope, "Body");
	s.writeStartElement(nsOnvifImaging, "SetImagingSettings");
	s.writeTextElement(nsOnvifImaging, "VideoSourceToken", m_selectedMedia.videoSourceToken);
	s.writeStartElement(nsOnvifImaging, "ImagingSettings");
	s.writeStartElement(nsOnvifSchema, "WhiteBalance");
	s.writeTextElement(nsOnvifSchema, "Mode", mode);
	s.writeEndElement(); // WhiteBalance
	s.writeEndElement(); // ImagingSettings
	s.writeEndElement(); // SetImagingSettings
	s.writeEndElement(); // Body
	s.writeEndElement(); // Envelope
	s.writeEndDocument();
	sendRequest(m_imagingXAddr, msg);
}

void PTZOnvif::applyImagingIfPending()
{
	if (!m_imagingDirty)
		return;
	if (m_imagingXAddr.isEmpty() || m_selectedMedia.videoSourceToken.isEmpty())
		return; /* try again once we're properly connected */
	if (!m_wbMode.isEmpty())
		imagingSetWhiteBalance(m_wbMode);
	m_imagingDirty = false;
}

void PTZOnvif::getSystemDateAndTime()
{
	/* GetSystemDateAndTime is required by the ONVIF spec to work without
	 * authentication, so we send a bare envelope (no WS-Security header).
	 * Use the response to align our WS-Security timestamps to the camera
	 * clock before any authenticated request goes out. */
	const QString hostformat("http://%1:%2/onvif/device_service");
	QString msg;
	QXmlStreamWriter s(&msg);
	writeStartOnvifDocument(s);
	s.writeStartElement(nsSoapEnvelope, "Envelope");
	s.writeStartElement(nsSoapEnvelope, "Body");
	s.writeEmptyElement(nsOnvifDevice, "GetSystemDateAndTime");
	s.writeEndElement(); // Body
	s.writeEndElement(); // Envelope
	s.writeEndDocument();
	sendRequest(hostformat.arg(host).arg(port), msg);
}

void PTZOnvif::getCapabilities()
{
	const QString hostformat("http://%1:%2/onvif/device_service");
	QString msg;
	QXmlStreamWriter s(&msg);
	writeStartOnvifDocument(s);
	s.writeStartElement(nsSoapEnvelope, "Envelope");
	writeHeader(s);
	s.writeStartElement(nsSoapEnvelope, "Body");
	s.writeStartElement(nsOnvifDevice, "GetCapabilities");
	s.writeTextElement(nsOnvifDevice, "Category", "All");
	s.writeEndElement(); // movetype
	s.writeEndElement(); // Body
	s.writeEndElement(); // Envelope
	s.writeEndDocument();
	sendRequest(hostformat.arg(host).arg(port), msg);
}

void PTZOnvif::ensureCapabilitiesRequested()
{
	if (m_capabilitiesRequested)
		return;
	m_capabilitiesRequested = true;
	getCapabilities();
}

void PTZOnvif::getProfiles()
{
	QString msg;
	QXmlStreamWriter s(&msg);
	writeStartOnvifDocument(s);
	s.writeStartElement(nsSoapEnvelope, "Envelope");
	writeHeader(s);
	s.writeStartElement(nsSoapEnvelope, "Body");
	s.writeEmptyElement(nsOnvifMedia, "GetProfiles");
	s.writeEndElement(); // Body
	s.writeEndElement(); // Envelope
	s.writeEndDocument();
	sendRequest(m_mediaXAddr, msg);
}

void PTZOnvif::requestFinished(QNetworkReply *reply)
{
	auto statusCodeV = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();

	m_isBusy = false;
	if (reply->error() > 0) {
		ptz_info("request error; message: %s, code: %i", QT_TO_UTF8(reply->errorString()), statusCodeV);
		++m_consecutiveFailures;
		if (m_consecutiveFailures >= 3 && isConnected())
			setConnected(false);
		/* If the very first request (GetSystemDateAndTime) fails we
		 * still need to try GetCapabilities; otherwise the device
		 * stays in a permanently un-initialized state. */
		ensureCapabilitiesRequested();
	} else {
		m_consecutiveFailures = 0;
		if (!isConnected())
			setConnected(true);
		handleResponse(reply->readAll());
	}
	do_update();
}

PTZOnvif::PTZOnvif(OBSData config) : PTZDevice(config)
{
	// for digest authenticaton request
	connect(&m_networkManager, SIGNAL(authenticationRequired(QNetworkReply *, QAuthenticator *)), this,
		SLOT(authRequired(QNetworkReply *, QAuthenticator *)));
	connect(&m_networkManager, SIGNAL(finished(QNetworkReply *)), this, SLOT(requestFinished(QNetworkReply *)));
	m_statusTimer.setInterval(5000);
	connect(&m_statusTimer, &QTimer::timeout, this, [this]() {
		/* When connected, keep position fresh; when disconnected and
		 * we've already passed the initial connect, retry from
		 * GetSystemDateAndTime so a rebooted camera that may have
		 * changed profile tokens / service URLs re-initializes cleanly. */
		if (isConnected()) {
			getStatus();
		} else if (m_consecutiveFailures >= 3) {
			m_capabilitiesRequested = false;
			m_timeOffsetSecs = 0;
			getSystemDateAndTime();
		}
	});
	m_statusTimer.start();
	set_config(config);
}

QString PTZOnvif::description()
{
	return QString("ONVIF %1@%2:%3").arg(username, host, QString::number(port));
}

void PTZOnvif::connectCamera()
{
	m_capabilitiesRequested = false;
	m_timeOffsetSecs = 0;
	m_pendingSetPresetSlot = -1;
	m_consecutiveFailures = 0;
	getSystemDateAndTime();
}

void PTZOnvif::do_update()
{
	if (pantilt_changed || zoom_changed) {
		if (pan_speed == 0.0 && tilt_speed == 0.0 && zoom_speed == 0.0) {
			pantilt_changed = false;
			zoom_changed = false;
			stop();
		} else if (!m_isBusy) {
			m_isBusy = true;
			pantilt_changed = false;
			zoom_changed = false;
			continuousMove(pan_speed * m_speed_boost, tilt_speed * m_speed_boost,
				       zoom_speed * m_speed_boost);
		}
	}
	if (focus_changed) {
		focus_changed = false;
		if (focus_speed == 0.0)
			imagingFocusStop();
		else
			imagingFocusMove(focus_speed * m_speed_boost);
	}
}

void PTZOnvif::set_autofocus(bool enabled)
{
	imagingSetAutoFocus(enabled);
}

void PTZOnvif::pantilt_abs(double pan, double tilt)
{
	absoluteMove(pan, tilt, 0.0);
}

void PTZOnvif::pantilt_rel(double pan, double tilt)
{
	relativeMove(pan, tilt, 0.0);
}

void PTZOnvif::pantilt_home()
{
	goToHomePosition();
}

void PTZOnvif::zoom_abs(double pos)
{
	absoluteMove(0.0, 0.0, pos);
}

void PTZOnvif::set_config(OBSData config)
{
	PTZDevice::set_config(config);
	host = obs_data_get_string(config, "host");
	port = (int)obs_data_get_int(config, "port");
	username = obs_data_get_string(config, "username");
	password = obs_data_get_string(config, "password");
	obs_data_set_default_double(config, "speed_boost", 1.0);
	m_speed_boost = obs_data_get_double(config, "speed_boost");
	if (m_speed_boost <= 0.0)
		m_speed_boost = 1.0;
	m_savedProfileToken = obs_data_get_string(config, "profile_token");
	QString newWb = obs_data_get_string(config, "wb_mode");
	if (newWb != m_wbMode) {
		m_wbMode = newWb;
		m_imagingDirty = true;
	}
	if (username == "")
		username = "admin";
	if (!port)
		port = 80;
	connectCamera();
}

OBSData PTZOnvif::get_config()
{
	OBSData config = PTZDevice::get_config();
	obs_data_set_string(config, "host", QT_TO_UTF8(host));
	obs_data_set_int(config, "port", port);
	obs_data_set_string(config, "username", QT_TO_UTF8(username));
	obs_data_set_string(config, "password", QT_TO_UTF8(password));
	obs_data_set_double(config, "speed_boost", m_speed_boost);
	obs_data_set_string(config, "profile_token", QT_TO_UTF8(m_selectedMedia.token));
	obs_data_set_string(config, "wb_mode", QT_TO_UTF8(m_wbMode));
	return config;
}

obs_properties_t *PTZOnvif::get_obs_properties()
{
	obs_properties_t *ptz_props = PTZDevice::get_obs_properties();
	obs_property_t *p = obs_properties_get(ptz_props, "interface");
	obs_properties_t *config = obs_property_group_content(p);
	obs_property_set_description(p, obs_module_text("PTZ.ONVIF.Name"));
	obs_properties_add_text(config, "warning", obs_module_text("PTZ.ONVIF.Warning"), OBS_TEXT_INFO);
	obs_properties_add_text(config, "host", obs_module_text("PTZ.Device.Hostname"), OBS_TEXT_DEFAULT);
	obs_properties_add_int(config, "port", obs_module_text("PTZ.Device.TCPPort"), 1, 65535, 1);
	obs_properties_add_text(config, "username", obs_module_text("PTZ.Device.Username"), OBS_TEXT_DEFAULT);
	obs_properties_add_text(config, "password", obs_module_text("PTZ.Device.Password"), OBS_TEXT_DEFAULT);
	obs_properties_add_float_slider(config, "speed_boost", obs_module_text("PTZ.ONVIF.SpeedBoost"), 0.1, 10.0,
					0.01);
	obs_property_t *prof = obs_properties_add_list(config, "profile_token",
						       obs_module_text("PTZ.ONVIF.MediaProfile"), OBS_COMBO_TYPE_LIST,
						       OBS_COMBO_FORMAT_STRING);
	if (m_mediaProfiles.isEmpty()) {
		obs_property_list_add_string(prof, obs_module_text("PTZ.ONVIF.NoProfilesYet"), "");
		obs_property_set_enabled(prof, false);
	} else {
		for (const auto &p : m_mediaProfiles) {
			QString label = p.name.isEmpty() ? p.token : p.name;
			obs_property_list_add_string(prof, QT_TO_UTF8(label), QT_TO_UTF8(p.token));
		}
	}

	/* Imaging: white balance mode. Cameras without an Imaging service
	 * simply ignore the request. */
	obs_property_t *wb = obs_properties_add_list(config, "wb_mode", obs_module_text("PTZ.WhiteBalance.Mode"),
						     OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_STRING);
	obs_property_list_add_string(wb, obs_module_text("PTZ.ONVIF.WbDefault"), "");
	obs_property_list_add_string(wb, obs_module_text("PTZ.WhiteBalance.Auto"), "AUTO");
	obs_property_list_add_string(wb, "Manual", "MANUAL");
	return ptz_props;
}
