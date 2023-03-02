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

void PTZOnvif::sendRequest(SoapRequest *soap_req)
{
	QNetworkRequest request(soap_req->host);
	request.setHeader(QNetworkRequest::ContentTypeHeader,
			  "application/soap+xml");

	QString concatenated = this->username + ":" + this->password;
	QByteArray data = concatenated.toLocal8Bit().toBase64();
	QString headerData = "Basic " + data;
	request.setRawHeader("Authorization", headerData.toLocal8Bit());

	soap_req->username = username;
	soap_req->password = password;
	m_networkManager.post(request, soap_req->createRequest().toUtf8());
}

void PTZOnvif::authRequired(QNetworkReply *, QAuthenticator *authenticator)
{
	authenticator->setUser(username);
	authenticator->setPassword(password);
}

QString SoapRequest::createRequest()
{
	QString request("<?xml version=\"1.0\" encoding=\"UTF-8\"?>");
	request.push_back(
		"<s:Envelope xmlns:s=\"http://www.w3.org/2003/05/soap-envelope\"");
	for (int i = 0; i < this->XMLNs.size(); i++)
		request.push_back(" " + this->XMLNs[i]);

	request.push_back(">");
	if (this->username != "" || this->action != "") {
		request.push_back("<s:Header>");
		if (this->action != "") {
			request.push_back(
				"<Action mustUnderstand=\"1\" xmlns=\"http://www.w3.org/2005/08/addressing\">");
			request.push_back(this->action);
			request.push_back("</Action>");
		}

		if (this->username != "")
			request.push_back(this->createUserToken());
		request.push_back("</s:Header>");
	}
	request += "<s:Body>" + this->body + "</s:Body>";
	request += "</s:Envelope>";
	return request;
}

QString SoapRequest::createUserToken()
{
	QUuid nonce = QUuid::createUuid();
	QString nonce64 = nonce.toByteArray().toBase64();
	auto timestamp = QDateTime::currentDateTimeUtc().toString(Qt::ISODate);
	auto token = nonce.toString() + timestamp + this->password;

	QCryptographicHash hash(QCryptographicHash::Sha1);
	hash.addData(token.toUtf8());
	QString hashTokenBase64 = hash.result().toBase64();

	QString result(
		"<Security s:mustUnderstand=\"1\" xmlns=\"http://docs.oasis-open.org/wss/2004/01/oasis-200401-wss-wssecurity-secext-1.0.xsd\">");
	result.push_back("<UsernameToken><Username>");
	result.push_back(this->username);
	result.push_back(
		"</Username><Password Type=\"http://docs.oasis-open.org/wss/2004/01/oasis-200401-wss-username-token-profile-1.0#PasswordDigest\">");
	result.push_back(hashTokenBase64);
	result.push_back(
		"</Password><Nonce EncodingType=\"http://docs.oasis-open.org/wss/2004/01/oasis-200401-wss-soap-message-security-1.0#Base64Binary\">");
	result.push_back(nonce64);
	result.push_back(
		"</Nonce><Created xmlns=\"http://docs.oasis-open.org/wss/2004/01/oasis-200401-wss-wssecurity-utility-1.0.xsd\">");
	result.push_back(timestamp);
	result.push_back("</Created></UsernameToken></Security>");
	return result;
}

const QList<QString> PTZOnvif::ptzNameSpace = {
	"xmlns:i=\"http://www.w3.org/2001/XMLSchema-instance\"",
	"xmlns:d=\"http://www.w3.org/2001/XMLSchema\"",
	"xmlns:c=\"http://www.w3.org/2003/05/soap-encoding\""};

SoapRequest *PTZOnvif::createSoapRequest()
{
	auto soapRequest = new SoapRequest();
	soapRequest->host = m_PTZAddress;
	soapRequest->XMLNs = ptzNameSpace;
	return soapRequest;
}

void PTZOnvif::continuousMove(double x, double y, double z)
{
	SoapRequest *soapRequest = createSoapRequest();
	soapRequest->action =
		"http://www.onvif.org/ver20/ptz/wsdl/ContinuousMove";
	QString body(
		"<ContinuousMove xmlns=\"http://www.onvif.org/ver20/ptz/wsdl\">");
	body.push_back("<ProfileToken>");
	body.push_back(m_selectedMedia.token);
	body.push_back("</ProfileToken>");
	body.push_back("<Velocity>");
	body.push_back(
		"<PanTilt xmlns=\"http://www.onvif.org/ver10/schema\" x=\"" +
		QString::number(x) + "\" y=\"" + QString::number(y) + "\"/>");
	body.push_back(
		"<Zoom xmlns=\"http://www.onvif.org/ver10/schema\" x=\"" +
		QString::number(z) + "\"/>");
	body.push_back("</Velocity></ContinuousMove>");
	soapRequest->body = body;
	sendRequest(soapRequest);
	delete soapRequest;
}

void PTZOnvif::absoluteMove(int x, int y, int z)
{
	SoapRequest *soapRequest = createSoapRequest();
	soapRequest->action =
		"http://www.onvif.org/ver20/ptz/wsdl/AbsoluteMove";
	QString body(
		"<AbsoluteMove xmlns=\"http://www.onvif.org/ver20/ptz/wsdl\">");
	body.push_back("<ProfileToken>");
	body.push_back(m_selectedMedia.token);
	body.push_back("</ProfileToken>");
	body.push_back("<Position>");
	body.push_back(
		"<PanTilt xmlns=\"http://www.onvif.org/ver10/schema\" x=\"" +
		QString::number(x) + "\" y=\"" + QString::number(y) + "\"/>");
	body.push_back(
		"<Zoom xmlns=\"http://www.onvif.org/ver10/schema\" x=\"" +
		QString::number(z) + "\"/>");
	body.push_back("</Position>");
	body.push_back("</AbsoluteMove>");
	soapRequest->body = body;
	sendRequest(soapRequest);
	delete soapRequest;
}

void PTZOnvif::relativeMove(int x, int y, int z)
{
	SoapRequest *soapRequest = createSoapRequest();
	soapRequest->action =
		"http://www.onvif.org/ver20/ptz/wsdl/RelativeMove";
	QString body(
		"<RelativeMove xmlns=\"http://www.onvif.org/ver20/ptz/wsdl\">");
	body.push_back("<ProfileToken>");
	body.push_back(m_selectedMedia.token);
	body.push_back("</ProfileToken>");
	body.push_back("<Translation>");
	body.push_back(
		"<PanTilt xmlns=\"http://www.onvif.org/ver10/schema\" x=\"" +
		QString::number(x) + "\" y=\"" + QString::number(y) + "\"/>");
	body.push_back(
		"<Zoom xmlns=\"http://www.onvif.org/ver10/schema\" x=\"" +
		QString::number(z) + "\"/>");
	body.push_back("</Translation>");
	body.push_back("</RelativeMove>");
	soapRequest->body = body;
	sendRequest(soapRequest);
	delete soapRequest;
}

void PTZOnvif::stop()
{
	SoapRequest *soapRequest = createSoapRequest();
	soapRequest->action = "http://www.onvif.org/ver20/ptz/wsdl/Stop";
	QString body("<Stop xmlns=\"http://www.onvif.org/ver20/ptz/wsdl\">");
	body.push_back("<ProfileToken>");
	body.push_back(m_selectedMedia.token);
	body.push_back("</ProfileToken>");
	body.push_back("<PanTilt>true</PanTilt><Zoom>true</Zoom>");
	body.push_back("</Stop>");
	soapRequest->body = body;
	sendRequest(soapRequest);
	delete soapRequest;
}

void PTZOnvif::goToHomePosition()
{
	SoapRequest *soapRequest = createSoapRequest();
	soapRequest->action =
		"http://www.onvif.org/ver20/ptz/wsdl/GotoHomePosition";
	QString body(
		"<GotoHomePosition xmlns=\"http://www.onvif.org/ver20/ptz/wsdl\">");
	body.push_back("<ProfileToken>");
	body.push_back(m_selectedMedia.token);
	body.push_back("</ProfileToken>");
	body.push_back("</GotoHomePosition>");
	soapRequest->body = body;
	sendRequest(soapRequest);
	delete soapRequest;
}

void PTZOnvif::memory_set(int i)
{
	QString preset = m_presetsModel.presetProperty(i, "token").toString();
	QString name = m_presetsModel.presetProperty(i, "name").toString();
	SoapRequest *soapRequest = createSoapRequest();
	soapRequest->action = "http://www.onvif.org/ver20/ptz/wsdl/SetPreset";
	QString body(
		"<SetPreset xmlns=\"http://www.onvif.org/ver20/ptz/wsdl\">");
	body.push_back("<ProfileToken>");
	body.push_back(m_selectedMedia.token);
	body.push_back("</ProfileToken>");
	if (name != "") {
		body.push_back("<PresetName>");
		body.push_back(name);
		body.push_back("</PresetName>");
	}
	if (preset != "") {
		body.push_back("<PresetToken>");
		body.push_back(preset);
		body.push_back("</PresetToken>");
	}
	body.push_back("</SetPreset>");
	soapRequest->body = body;
	sendRequest(soapRequest);
	delete soapRequest;
}

void PTZOnvif::memory_reset(int i)
{
	QString preset = m_presetsModel.presetProperty(i, "token").toString();
	if (preset == "")
		return;
	SoapRequest *soapRequest = createSoapRequest();
	soapRequest->action =
		"http://www.onvif.org/ver20/ptz/wsdl/RemovePreset";
	QString body(
		"<RemovePreset xmlns=\"http://www.onvif.org/ver20/ptz/wsdl\">");
	body.push_back("<ProfileToken>");
	body.push_back(m_selectedMedia.token);
	body.push_back("</ProfileToken>");
	body.push_back("<PresetToken>");
	body.push_back(preset);
	body.push_back("</PresetToken>");
	body.push_back("</RemovePreset>");
	soapRequest->body = body;
	sendRequest(soapRequest);
	delete soapRequest;
}

void PTZOnvif::memory_recall(int i)
{
	QString preset = m_presetsModel.presetProperty(i, "token").toString();
	if (preset == "")
		return;
	SoapRequest *soapRequest = createSoapRequest();
	soapRequest->action = "http://www.onvif.org/ver20/ptz/wsdl/GotoPreset";
	QString body(
		"<GotoPreset xmlns=\"http://www.onvif.org/ver20/ptz/wsdl\">");
	body.push_back("<ProfileToken>");
	body.push_back(m_selectedMedia.token);
	body.push_back("</ProfileToken>");
	body.push_back("<PresetToken>");
	body.push_back(preset);
	body.push_back("</PresetToken>");
	body.push_back("</GotoPreset>");
	soapRequest->body = body;
	sendRequest(soapRequest);
	delete soapRequest;
}

void PTZOnvif::getPresets()
{
	SoapRequest *soapRequest = createSoapRequest();
	soapRequest->action = "http://www.onvif.org/ver20/ptz/wsdl/GetPresets";
	QString body(
		"<GetPresets xmlns=\"http://www.onvif.org/ver20/ptz/wsdl\">");
	body.push_back("<ProfileToken>");
	body.push_back(m_selectedMedia.token);
	body.push_back("</ProfileToken>");
	body.push_back("</GetPresets>");
	soapRequest->body = body;
	QString response;

	sendRequest(soapRequest);
	delete soapRequest;
}

void PTZOnvif::handleResponse(QString response)
{
	QDomDocument doc;
	doc.setContent(response);

	auto nl = doc.elementsByTagName("tds:GetCapabilitiesResponse");
	for (int i = 0; i < nl.length(); i++)
		handleGetCapabilitiesResponse(nl.at(i));
	nl = doc.elementsByTagName("trt:GetProfilesResponse");
	for (int i = 0; i < nl.length(); i++)
		handleGetProfilesResponse(nl.at(i));
	handleGetPresetsResponse(doc);
}

void PTZOnvif::handleGetPresetsResponse(QDomDocument doc)
{
	QDomNodeList nodes = doc.elementsByTagName("tptz:Preset");

	for (int i = 0; i < nodes.length(); i++) {
		auto node = nodes.at(i);
		QString token;
		QString name;

		for (int a = 0; a < node.attributes().length(); a++) {
			auto item = node.attributes().item(a);
			if (item.nodeName() == "token")
				token = item.nodeValue();
		}

		for (int c = 0; c < node.childNodes().length(); c++) {
			auto child = node.childNodes().at(c);
			if (child.nodeName() == "tt:Name")
				name = child.toElement().text();
		}

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
	static const QList<QString> deviceNameSpace = {
		"xmlns:tds=\"http://www.onvif.org/ver10/device/wsdl\"",
		"xmlns:tt=\"http://www.onvif.org/ver10/schema\""};
	SoapRequest *soapRequest = new SoapRequest();
	soapRequest->host =
		tr("http://%1:%2/onvif/device_service").arg(host).arg(port);
	soapRequest->XMLNs = deviceNameSpace;
	QString body(
		"<tds:GetCapabilities><tds:Category>All</tds:Category></tds:GetCapabilities>");
	soapRequest->body = body;
	sendRequest(soapRequest);
	delete soapRequest;
}

void PTZOnvif::handleGetCapabilitiesResponse(QDomNode node)
{
	auto responseElement = node.toElement();
	for (auto n2 = responseElement.firstChild(); !n2.isNull();
	     n2 = n2.nextSibling()) {
		QDomElement elementCapabilities = n2.toElement();
		for (QDomNode n3 = elementCapabilities.firstChild();
		     !n3.isNull(); n3 = n3.nextSibling()) {
			if (n3.nodeName().toLower().endsWith("ptz")) {
				QDomElement elementPtz = n3.toElement();
				for (QDomNode n4 = elementPtz.firstChild();
				     !n4.isNull(); n4 = n4.nextSibling()) {
					if (n4.nodeName().toLower().endsWith(
						    "xaddr"))
						m_PTZAddress =
							n4.toElement().text();
				}
			}
			if (n3.nodeName().toLower().endsWith("media")) {
				QDomElement elementPtz = n3.toElement();
				for (QDomNode n4 = elementPtz.firstChild();
				     !n4.isNull(); n4 = n4.nextSibling()) {
					if (n4.nodeName().toLower().endsWith(
						    "xaddr"))
						m_mediaXAddr =
							n4.toElement().text();
				}
			}
		}
	}
	getProfiles();
}

void PTZOnvif::getProfiles()
{
	static const QList<QString> mediaNameSpace = {
		"xmlns:trt=\"http://www.onvif.org/ver10/media/wsdl\"",
		"xmlns:tt=\"http://www.onvif.org/ver10/schema\""};
	SoapRequest *soapRequest = new SoapRequest();
	soapRequest->host = m_mediaXAddr;
	soapRequest->XMLNs = mediaNameSpace;
	QString body("<trt:GetProfiles/>");
	soapRequest->body = body;
	sendRequest(soapRequest);
	delete soapRequest;
}

void PTZOnvif::handleGetProfilesResponse(QDomNode node)
{
	QList<MediaProfile> result;
	QDomElement elementGetProfilesResponse = node.toElement();
	for (QDomNode node2 = elementGetProfilesResponse.firstChild();
	     !node2.isNull(); node2 = node2.nextSibling()) {
		MediaProfile profile;
		QDomElement elementProfiles = node2.toElement();
		for (int i = 0; i < node2.attributes().size(); i++) {
			if (node2.attributes().item(i).nodeName() == "token")
				profile.token =
					node2.attributes().item(i).nodeValue();
		}

		for (QDomNode node3 = elementProfiles.firstChild();
		     !node3.isNull(); node3 = node3.nextSibling()) {
			if (node3.nodeName().toLower().endsWith("name"))
				profile.name = node3.toElement().text();
		}
		result.push_back(profile);
	}
	if (!result.isEmpty())
		m_selectedMedia = result[0];
	getPresets();
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
	obs_properties_add_text(
		config, "warning",
		"Warning: ONVIF support is experimental and may cause the OBS user interface to freeze. Use at your own risk.",
		OBS_TEXT_INFO);
	obs_properties_add_text(config, "host", "IP Host", OBS_TEXT_DEFAULT);
	obs_properties_add_int(config, "port", "TCP port", 1, 65535, 1);
	obs_properties_add_text(config, "username", "Username",
				OBS_TEXT_DEFAULT);
	obs_properties_add_text(config, "password", "Password",
				OBS_TEXT_DEFAULT);
	return ptz_props;
}
