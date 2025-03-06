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

#include "imported/qt-wrappers.hpp"
#include "ptz-onvif.hpp"
#include <QtXml/QDomDocument>
#include <QXmlStreamWriter>

void PTZOnvif::sendRequest(QString url, QString req)
{
	QNetworkRequest request(url);
	request.setHeader(QNetworkRequest::ContentTypeHeader,
			  "application/soap+xml");
	QString concatenated = username + ":" + password;
	QByteArray data = concatenated.toLocal8Bit().toBase64();
	QString headerData = "Basic " + data;
	request.setRawHeader("Authorization", headerData.toLocal8Bit());
	m_networkManager.post(request, req.toUtf8());
}

void PTZOnvif::authRequired(QNetworkReply *, QAuthenticator *authenticator)
{
	authenticator->setUser(username);
	authenticator->setPassword(password);
}

const QString nsXmlSchema("http://www.w3.org/2001/XMLSchema"); //xsd
const QString
	nsXmlSchemaInstance("http://www.w3.org/2001/XMLSchema-instance"); //xsi
const QString
	nsSoapEnvelope("http://www.w3.org/2003/05/soap-envelope"); //SOAP-ENV
const QString
	nsSoapEncoding("http://www.w3.org/2003/05/soap-encoding");  //SOAP-ENC
const QString nsAddressing("http://www.w3.org/2005/08/addressing"); //wsa5
const QString nsOnvifSchema("http://www.onvif.org/ver10/schema");   //tt
const QString nsOnvifDevice("http://www.onvif.org/ver10/device/wsdl"); //tds
const QString nsOnvifMedia("http://www.onvif.org/ver10/media/wsdl");   //trt
const QString nsOnvifPtz("http://www.onvif.org/ver20/ptz/wsdl");       //tptz
const QString nsWssSecext(
	"http://docs.oasis-open.org/wss/2004/01/oasis-200401-wss-wssecurity-secext-1.0.xsd"); //wsse
const QString nsWssUtility(
	"http://docs.oasis-open.org/wss/2004/01/oasis-200401-wss-wssecurity-utility-1.0.xsd"); //wsu
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
	auto timestamp = QDateTime::currentDateTimeUtc().toString(Qt::ISODate);
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

void PTZOnvif::genericMove(QString movetype, QString property, double x,
			   double y, double z)
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

void PTZOnvif::absoluteMove(int x, int y, int z)
{
	genericMove("AbsoluteMove", "Position", x, y, z);
}

void PTZOnvif::relativeMove(int x, int y, int z)
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

void PTZOnvif::handleResponse(QString response)
{
	QDomDocument doc;
	QDomNodeList nl;

#if QT_VERSION >= QT_VERSION_CHECK(6, 0, 0)
    QDomDocument::ParseOptions options = QDomDocument::ParseOption::UseNamespaceProcessing;
    doc.setContent(QAnyStringView(response), options);
#else
	doc.setContent(response, true);
#endif

	nl = doc.elementsByTagNameNS(nsOnvifDevice, "GetCapabilitiesResponse");
	for (int i = 0; i < nl.length(); i++)
		handleGetCapabilitiesResponse(nl.at(i));
	nl = doc.elementsByTagNameNS(nsOnvifMedia, "GetProfilesResponse");
	for (int i = 0; i < nl.length(); i++)
		handleGetProfilesResponse(nl.at(i));
	handleGetPresetsResponse(doc);
}

void PTZOnvif::handleGetCapabilitiesResponse(QDomNode node)
{
	auto responseElement = node.toElement();
	auto pl = responseElement.elementsByTagNameNS(nsOnvifSchema, "PTZ");
	for (int i = 0; i < pl.length(); i++) {
		auto e = pl.at(i).toElement();
		m_PTZAddress =
			e.firstChildElement("XAddr", nsOnvifSchema).text();
	}

	pl = responseElement.elementsByTagNameNS(nsOnvifSchema, "Media");
	for (int i = 0; i < pl.length(); i++) {
		auto e = pl.at(i).toElement();
		m_mediaXAddr =
			e.firstChildElement("XAddr", nsOnvifSchema).text();
	}
	getProfiles();
}

void PTZOnvif::handleGetProfilesResponse(QDomNode n)
{
	QList<MediaProfile> result;
	auto pl = n.toElement().elementsByTagNameNS(nsOnvifMedia, "Profiles");
	for (int i = 0; i < pl.length(); i++) {
		MediaProfile pro;
		auto e = pl.at(i).toElement();
		pro.token = e.attribute("token");
		pro.name = e.firstChildElement("Name", nsOnvifSchema).text();
		result.push_back(pro);
	}
	if (!result.isEmpty())
		m_selectedMedia = result[0];
	getPresets();
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
	auto statusCodeV =
		reply->attribute(QNetworkRequest::HttpStatusCodeAttribute)
			.toInt();

	m_isBusy = false;
	if (reply->error() > 0) {
		ptz_info("request error; message: %s, code: %i",
			 QT_TO_UTF8(reply->errorString()), statusCodeV);
	} else {
		handleResponse(reply->readAll());
	}
	do_update();
}

PTZOnvif::PTZOnvif(OBSData config) : PTZDevice(config)
{
	// for digest authenticaton request
	connect(&m_networkManager,
		SIGNAL(authenticationRequired(QNetworkReply *,
					      QAuthenticator *)),
		this, SLOT(authRequired(QNetworkReply *, QAuthenticator *)));
	connect(&m_networkManager, SIGNAL(finished(QNetworkReply *)), this,
		SLOT(requestFinished(QNetworkReply *)));
	set_config(config);
}

QString PTZOnvif::description()
{
	return QString("ONVIF %1@%2:%3")
		.arg(username, host, QString::number(port));
}

void PTZOnvif::connectCamera()
{
	getCapabilities();
}

void PTZOnvif::do_update()
{
	if (status &
	    (STATUS_PANTILT_SPEED_CHANGED | STATUS_ZOOM_SPEED_CHANGED)) {
		if (pan_speed == 0.0 && tilt_speed == 0.0 &&
		    zoom_speed == 0.0) {
			status &= ~(STATUS_PANTILT_SPEED_CHANGED |
				    STATUS_ZOOM_SPEED_CHANGED);
			stop();
		} else if (!m_isBusy) {
			m_isBusy = true;
			status &= ~(STATUS_PANTILT_SPEED_CHANGED |
				    STATUS_ZOOM_SPEED_CHANGED);
			continuousMove(pan_speed, tilt_speed, zoom_speed);
		}
	}
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
	if (username == "")
		username = "admin";
	if (!port)
		port = 8899;
	connectCamera();
}

OBSData PTZOnvif::get_config()
{
	OBSData config = PTZDevice::get_config();
	obs_data_set_string(config, "host", QT_TO_UTF8(host));
	obs_data_set_int(config, "port", port);
	obs_data_set_string(config, "username", QT_TO_UTF8(username));
	obs_data_set_string(config, "password", QT_TO_UTF8(password));
	return config;
}

obs_properties_t *PTZOnvif::get_obs_properties()
{
	obs_properties_t *ptz_props = PTZDevice::get_obs_properties();
	obs_property_t *p = obs_properties_get(ptz_props, "interface");
	obs_properties_t *config = obs_property_group_content(p);
	obs_property_set_description(p, "ONVIF Connection (Experimental)");
	obs_properties_add_text(config, "warning",
				"Warning: ONVIF support is experimental",
				OBS_TEXT_INFO);
	obs_properties_add_text(config, "host", "IP Host", OBS_TEXT_DEFAULT);
	obs_properties_add_int(config, "port", "TCP port", 1, 65535, 1);
	obs_properties_add_text(config, "username", "Username",
				OBS_TEXT_DEFAULT);
	obs_properties_add_text(config, "password", "Password",
				OBS_TEXT_DEFAULT);
	return ptz_props;
}
