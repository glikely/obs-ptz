/* Pan Tilt Zoom Onvif implementation
 *
 * Copyright 2022 Jonatã Bolzan Loss <jonata@jonata.org>
 *
 * SPDX-License-Identifier: GPLv2
 */

#include "imported/qt-wrappers.hpp"
#include "ptz-onvif.hpp"
#include <QtXml/QDomDocument>

PTZOnvif::PTZOnvif(OBSData config)
	: PTZDevice(config)
{
	set_config(config);
	auto_settings_filter += {"port", "host", "username", "password"};
}

void PTZOnvif::connectCamera()
{
    OnvifDeviceService a;
    DeviceCapabilities deviceCapabilities = a.GetCapabilities(tr("http://%1:%2/onvif/device_service").arg(host).arg(port), username, password);

    OnvifMediaService b;
    QList<MediaProfile> listProfile = b.GetProfiles(deviceCapabilities.mediaXAddr,  username, password);

    if(listProfile.isEmpty())
    {
        qInfo() << "[PTZOnvif] Connection failed ";
        return;
    }
    m_PTZAddress = deviceCapabilities.ptzXAddr;
    m_selectedMedia = listProfile[0];

    qInfo() << "[PTZOnvif] Connection success ";
}

void PTZOnvif::settingsChanged()
{
    printf("settingsChanged\n");
}

void PTZOnvif::pantilt(double pan, double tilt)
{
    float motionless = 0.0;
    OnvifPTZService c;
    if (pan == motionless && tilt == motionless)
    {
        QThread::msleep(200);
        c.Stop(m_PTZAddress, username, password, m_selectedMedia.token);
    }
    else {
        c.ContinuousMove(m_PTZAddress, username, password, m_selectedMedia.token, -pan, tilt, motionless);
    }
}

void PTZOnvif::pantilt_abs(int pan, int tilt)
{
    float motionless = 0.0;
    OnvifPTZService c;
    c.AbsoluteMove(m_PTZAddress, username, password, m_selectedMedia.token, pan, tilt, motionless);

}

void PTZOnvif::pantilt_rel(int pan, int tilt)
{
    float motionless = 0.0;
    OnvifPTZService c;
    c.RelativeMove(m_PTZAddress, username, password, m_selectedMedia.token, pan, tilt, motionless);

}

void PTZOnvif::pantilt_home()
{
    OnvifPTZService c;
    c.GoToHomePosition(m_PTZAddress, username, password, m_selectedMedia.token);
}

void PTZOnvif::zoom(double speed)
{
    float motionless = 0.0;
    OnvifPTZService c;
    if (speed == motionless)
    {
        QThread::msleep(200);
        c.Stop(m_PTZAddress, username, password, m_selectedMedia.token);
    }
    else {
        c.ContinuousMove(m_PTZAddress, username, password, m_selectedMedia.token, motionless, motionless, speed);
    }
}

void PTZOnvif::zoom_abs(int pos)
{
    float motionless = 0.0;
    OnvifPTZService c;
    c.AbsoluteMove(m_PTZAddress, username, password, m_selectedMedia.token, motionless, motionless, pos);
}

SoapRequest::SoapRequest() : QObject (nullptr)
{
    networkManager = new QNetworkAccessManager();
}

SoapRequest::~SoapRequest()
{
    delete networkManager;
}

bool SoapRequest::sendRequest(QString &result)
{
    QNetworkRequest request(this->host);
    request.setHeader(QNetworkRequest::ContentTypeHeader, "application/soap+xml");

    QString concatenated = this->username + ":" + this->password;
    QByteArray data = concatenated.toLocal8Bit().toBase64();
    QString headerData = "Basic " + data;
    request.setRawHeader("Authorization", headerData.toLocal8Bit());

    // qInfo() << "[PTZOnvif] Request onvif " << this->createRequest();

    //for digest authenticaton request
    QObject::connect(this->networkManager, SIGNAL(authenticationRequired(QNetworkReply*,QAuthenticator*)), this, SLOT(authRequired(QNetworkReply *, QAuthenticator *)));
    QNetworkReply * reply = this->networkManager->post(request, this->createRequest().toUtf8());

    QTimer timerTimeout;
    timerTimeout.setSingleShot(true);
    QEventLoop loop;
    loop.connect(&timerTimeout, SIGNAL(timeout()), SLOT(quit()));
    loop.connect(this->networkManager, SIGNAL(finished(QNetworkReply*)), SLOT(quit()));

    timerTimeout.start(3000);
    loop.exec();

    if (timerTimeout.isActive())
    {
        timerTimeout.stop();
        if(reply->error() > 0)
        {
            qInfo() << "[PTZOnvif] Request error " << reply->error();
        }
        else
        {
            QVariant statusCodeV = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute);
            result = QString(reply->readAll());
            if (statusCodeV.toInt() == 200)
            {
                return true;
            }
        }
    } else
    {
       disconnect(reply, SIGNAL(finished()), &loop, SLOT(quit()));
       reply->abort();
       qInfo() << "[PTZOnvif] Request timeout";
    }
    return false;
}

void SoapRequest::authRequired(QNetworkReply *, QAuthenticator *authenticator)
{
    authenticator->setUser(this->username);
    authenticator->setPassword(this->password);
}

QString SoapRequest::createRequest()
{
    QString request("<?xml version=\"1.0\" encoding=\"UTF-8\"?>");
    request.push_back("<s:Envelope xmlns:s=\"http://www.w3.org/2003/05/soap-envelope\"");
    for (int i = 0; i < this->XMLNs.size(); i++)
    {
        request.push_back(" " + this->XMLNs[i]);
    }
    request.push_back(">");
    if (this->username != "" || this->action != "")
    {
        request.push_back("<s:Header>");
        if (this->action != "")
        {
            request.push_back("<Action mustUnderstand=\"1\" xmlns=\"http://www.w3.org/2005/08/addressing\">");
            request.push_back(this->action);
            request.push_back("</Action>");
        }

        if (this->username != "")
        {
            request.push_back(this->createUserToken());
        }
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

    QString result("<Security s:mustUnderstand=\"1\" xmlns=\"http://docs.oasis-open.org/wss/2004/01/oasis-200401-wss-wssecurity-secext-1.0.xsd\">");
    result.push_back("<UsernameToken><Username>");
    result.push_back(this->username);
    result.push_back("</Username><Password Type=\"http://docs.oasis-open.org/wss/2004/01/oasis-200401-wss-username-token-profile-1.0#PasswordDigest\">");
    result.push_back(hashTokenBase64);
    result.push_back("</Password><Nonce EncodingType=\"http://docs.oasis-open.org/wss/2004/01/oasis-200401-wss-soap-message-security-1.0#Base64Binary\">");
    result.push_back(nonce64);
    result.push_back("</Nonce><Created xmlns=\"http://docs.oasis-open.org/wss/2004/01/oasis-200401-wss-wssecurity-utility-1.0.xsd\">");
    result.push_back(timestamp);
    result.push_back("</Created></UsernameToken></Security>");
    return result;
}

OnvifPTZService::OnvifPTZService()
{
    this->ptzNameSpace.push_back("xmlns:i=\"http://www.w3.org/2001/XMLSchema-instance\"");
    this->ptzNameSpace.push_back("xmlns:d=\"http://www.w3.org/2001/XMLSchema\"");
    this->ptzNameSpace.push_back("xmlns:c=\"http://www.w3.org/2003/05/soap-encoding\"");
}


bool OnvifPTZService::ContinuousMove(QString host, QString username, QString password, QString profile, double x, double y, double z)
{
    SoapRequest *soapRequest = new SoapRequest();
    soapRequest->host = host;
    soapRequest->username = username;
    soapRequest->password = password;
    soapRequest->action = "http://www.onvif.org/ver20/ptz/wsdl/ContinuousMove";
    soapRequest->XMLNs = this->ptzNameSpace;
    QString body("<ContinuousMove xmlns=\"http://www.onvif.org/ver20/ptz/wsdl\">");
    body.push_back("<ProfileToken>");
    body.push_back(profile);
    body.push_back("</ProfileToken>");
    body.push_back("<Velocity>");
    body.push_back("<PanTilt xmlns=\"http://www.onvif.org/ver10/schema\" x=\"" + QString::number(x) + "\" y=\""+ QString::number(y) +"\"/>");
    body.push_back("<Zoom xmlns=\"http://www.onvif.org/ver10/schema\" x=\"" + QString::number(z) + "\"/>");
    body.push_back("</Velocity></ContinuousMove>");
    soapRequest->body = body;
    QString response;
    bool result = soapRequest->sendRequest(response);
    // qInfo() << "[OnvifPTZService] ContinuousMove Response " << response;
    delete soapRequest;
    return result;
}


bool OnvifPTZService::AbsoluteMove(QString host, QString username, QString password, QString profile, double x, double y, double z)
{
    SoapRequest *soapRequest = new SoapRequest();
    soapRequest->host = host;
    soapRequest->username = username;
    soapRequest->password = password;
    soapRequest->action = "http://www.onvif.org/ver20/ptz/wsdl/ContinuousMove";
    soapRequest->XMLNs = this->ptzNameSpace;
    QString body("<AbsoluteMove xmlns=\"http://www.onvif.org/ver20/ptz/wsdl\">");
    body.push_back("<ProfileToken>");
    body.push_back(profile);
    body.push_back("</ProfileToken>");
    body.push_back("<Position>");
    body.push_back("<PanTilt xmlns=\"http://www.onvif.org/ver10/schema\" x=\"" + QString::number(x) + "\" y=\""+ QString::number(y) +"\"/>");
    body.push_back("<Zoom xmlns=\"http://www.onvif.org/ver10/schema\" x=\"" + QString::number(z) + "\"/>");
    body.push_back("</Position>");
    // body.push_back("<Speed>");
    // body.push_back("<PanTilt xmlns=\"http://www.onvif.org/ver10/schema\" x=\"" + QString::number(x) + "\" y=\""+ QString::number(y) +"\"/>");
    // body.push_back("<Zoom xmlns=\"http://www.onvif.org/ver10/schema\" x=\"" + QString::number(z) + "\"/>");
    // body.push_back("</Speed>");
    body.push_back("</AbsoluteMove>");
    soapRequest->body = body;
    QString response;
    bool result = soapRequest->sendRequest(response);
    // qInfo() << "[OnvifPTZService] AbsoluteMove Response " << response;
    delete soapRequest;
    return result;
}

bool OnvifPTZService::RelativeMove(QString host, QString username, QString password, QString profile, double x, double y, double z)
{
    SoapRequest *soapRequest = new SoapRequest();
    soapRequest->host = host;
    soapRequest->username = username;
    soapRequest->password = password;
    soapRequest->action = "http://www.onvif.org/ver20/ptz/wsdl/ContinuousMove";
    soapRequest->XMLNs = this->ptzNameSpace;
    QString body("<RelativeMove xmlns=\"http://www.onvif.org/ver20/ptz/wsdl\">");
    body.push_back("<ProfileToken>");
    body.push_back(profile);
    body.push_back("</ProfileToken>");
    body.push_back("<Translation>");
    body.push_back("<PanTilt xmlns=\"http://www.onvif.org/ver10/schema\" x=\"" + QString::number(x) + "\" y=\""+ QString::number(y) +"\"/>");
    body.push_back("<Zoom xmlns=\"http://www.onvif.org/ver10/schema\" x=\"" + QString::number(z) + "\"/>");
    body.push_back("</Translation>");
    // body.push_back("<Speed>");
    // body.push_back("<PanTilt xmlns=\"http://www.onvif.org/ver10/schema\" x=\"" + QString::number(x) + "\" y=\""+ QString::number(y) +"\"/>");
    // body.push_back("<Zoom xmlns=\"http://www.onvif.org/ver10/schema\" x=\"" + QString::number(z) + "\"/>");
    // body.push_back("</Speed>");
    body.push_back("</RelativeMove>");
    soapRequest->body = body;
    QString response;
    bool result = soapRequest->sendRequest(response);
    // qInfo() << "[OnvifPTZService] RelativeMove Response " << response;
    delete soapRequest;
    return result;
}

bool OnvifPTZService::Stop(QString host, QString username, QString password, QString profile)
{
    SoapRequest *soapRequest = new SoapRequest();
    soapRequest->host = host;
    soapRequest->username = username;
    soapRequest->password = password;
    soapRequest->action = "http://www.onvif.org/ver20/ptz/wsdl/Stop";
    soapRequest->XMLNs = this->ptzNameSpace;
    QString body("<Stop xmlns=\"http://www.onvif.org/ver20/ptz/wsdl\">");
    body.push_back("<ProfileToken>");
    body.push_back(profile);
    body.push_back("</ProfileToken>");
    body.push_back("<PanTilt>true</PanTilt><Zoom>true</Zoom>");
    body.push_back("</Stop>");
    soapRequest->body = body;
    QString response;
    bool result = soapRequest->sendRequest(response);
    // qInfo() << "[OnvifPTZService] Stop Response " << response;
    delete soapRequest;
    return result;
}

bool OnvifPTZService::GoToHomePosition(QString host, QString username, QString password, QString profile)
{
    SoapRequest *soapRequest = new SoapRequest();
    soapRequest->host = host;
    soapRequest->username = username;
    soapRequest->password = password;
    soapRequest->action = "http://www.onvif.org/ver20/ptz/wsdl/GotoHomePosition";
    soapRequest->XMLNs = this->ptzNameSpace;
    QString body("<GotoHomePosition xmlns=\"http://www.onvif.org/ver20/ptz/wsdl\">");
    body.push_back("<ProfileToken>");
    body.push_back(profile);
    body.push_back("</ProfileToken>");
    body.push_back("</GotoHomePosition>");
    soapRequest->body = body;
    QString response;
    bool result = soapRequest->sendRequest(response);
    // qInfo() << "[OnvifPTZService] GoToHomePosition Response " << response;
    delete soapRequest;
    return result;
}

OnvifDeviceService::OnvifDeviceService()
{
    this->deviceNameSpace.push_back("xmlns:tds=\"http://www.onvif.org/ver10/device/wsdl\"");
    this->deviceNameSpace.push_back("xmlns:tt=\"http://www.onvif.org/ver10/schema\"");
}

DeviceCapabilities OnvifDeviceService::GetCapabilities(QString deviceXAddress, QString username, QString password)
{
    SoapRequest *soapRequest = new SoapRequest();
    soapRequest->host = deviceXAddress;
    soapRequest->username = username;
    soapRequest->password = password;
    soapRequest->XMLNs = this->deviceNameSpace;
    QString body("<tds:GetCapabilities><tds:Category>All</tds:Category></tds:GetCapabilities>");
    soapRequest->body = body;
    QString response;
    bool ok = soapRequest->sendRequest(response);
    // qInfo() << "[OnvifDeviceService] GetPTZXAddress Response " << response;
    delete soapRequest;

    DeviceCapabilities result;
    if (ok)
    {
        QDomDocument doc;
        doc.setContent(response);
        QDomElement rootElement = doc.documentElement();
        for(QDomNode node = rootElement.firstChild(); !node.isNull(); node = node.nextSibling())
        {
            QDomElement elementBody = node.toElement();
            for(QDomNode node1 = elementBody.firstChild(); !node1.isNull(); node1 = node1.nextSibling())
            {
                QDomElement elementGetCapabilitiesResponse = node1.toElement();
                for(QDomNode node2 = elementGetCapabilitiesResponse.firstChild(); !node2.isNull(); node2 = node2.nextSibling())
                {
                    QDomElement elementCapabilities = node2.toElement();
                    for(QDomNode node3 = elementCapabilities.firstChild(); !node3.isNull(); node3 = node3.nextSibling())
                    {
                        if (node3.nodeName().toLower().endsWith("ptz"))
                        {
                            QDomElement elementPtz = node3.toElement();
                            for(QDomNode node4 = elementPtz.firstChild(); !node4.isNull(); node4 = node4.nextSibling())
                            {
                                if (node4.nodeName().toLower().endsWith("xaddr"))
                                {
                                    result.ptzXAddr = node4.toElement().text();
                                }
                            }
                        }
                        else if (node3.nodeName().toLower().endsWith("media"))
                        {
                            QDomElement elementPtz = node3.toElement();
                            for(QDomNode node4 = elementPtz.firstChild(); !node4.isNull(); node4 = node4.nextSibling())
                            {
                                if (node4.nodeName().toLower().endsWith("xaddr"))
                                {
                                    result.mediaXAddr = node4.toElement().text();
                                }
                            }
                        }
                    }
                }
            }
        }
    }
    return result;
}

OnvifMediaService::OnvifMediaService()
{
    this->mediaNameSpace.push_back("xmlns:trt=\"http://www.onvif.org/ver10/media/wsdl\"");
    this->mediaNameSpace.push_back("xmlns:tt=\"http://www.onvif.org/ver10/schema\"");
}

QList<MediaProfile> OnvifMediaService::GetProfiles(QString mediaXAddress, QString username, QString password)
{
    SoapRequest *soapRequest = new SoapRequest();
    soapRequest->host = mediaXAddress;
    soapRequest->username = username;
    soapRequest->password = password;
    soapRequest->XMLNs = this->mediaNameSpace;
    QString body("<trt:GetProfiles/>");
    soapRequest->body = body;
    QString response;
    bool ok = soapRequest->sendRequest(response);
    // qInfo() << "[OnvifMediaService] GetProfiles Response " << response;
    delete soapRequest;

    QList<MediaProfile> result;
    if (ok) {
        QDomDocument doc;
        doc.setContent(response);
        QDomElement rootElement = doc.documentElement();
        for(QDomNode node = rootElement.firstChild(); !node.isNull(); node = node.nextSibling())
        {
            QDomElement elementBody = node.toElement();
            for(QDomNode node1 = elementBody.firstChild(); !node1.isNull(); node1 = node1.nextSibling())
            {
                QDomElement elementGetProfilesResponse = node1.toElement();
                for(QDomNode node2 = elementGetProfilesResponse.firstChild(); !node2.isNull(); node2 = node2.nextSibling())
                {
                    MediaProfile profile;
                    QDomElement elementProfiles= node2.toElement();
                    for (int i = 0; i < node2.attributes().size(); i++)
                    {
                        if (node2.attributes().item(i).nodeName() == "token") {
                            profile.token = node2.attributes().item(i).nodeValue();
                        }
                    }
                    for(QDomNode node3 = elementProfiles.firstChild(); !node3.isNull(); node3 = node3.nextSibling())
                    {
                        if (node3.nodeName().toLower().endsWith("name")) {
                            profile.name = node3.toElement().text();
                        }
                    }
                    result.push_back(profile);
                }
            }
        }
    }
    return result;
}

void PTZOnvif::set_config(OBSData config)
{
	PTZDevice::set_config(config);
	host = obs_data_get_string(config, "host");
	port = obs_data_get_int(config, "port");
    username = obs_data_get_string(config, "username");
    password = obs_data_get_string(config, "password");
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
	obs_properties_t *props = PTZDevice::get_obs_properties();
	obs_property_t *p = obs_properties_get(props, "interface");
	obs_properties_t *config = obs_property_group_content(p);
	obs_property_set_description(p, "Onvif Connection");
	obs_properties_add_text(config, "host", "IP Host", OBS_TEXT_DEFAULT);
	obs_properties_add_int(config, "port", "TCP port", 1, 65535, 1);
	obs_properties_add_text(config, "username", "Username", OBS_TEXT_DEFAULT);
	obs_properties_add_text(config, "password", "Password", OBS_TEXT_DEFAULT);
	return props;
}