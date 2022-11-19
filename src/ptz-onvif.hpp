/* Pan Tilt Zoom ONVIF Implementation
 *
 * Copyright 2022 Jonatã Bolzan Loss <jonata@jonata.org>
 *
 * SPDX-License-Identifier: GPLv2
 */
#pragma once

#include <QObject>
#include "ptz-device.hpp"
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QObject>
#include <QUuid>
#include <QAuthenticator>
#include <QDateTime>
#include <QEventLoop>
#include <QTimer>
#include <QList>

class MediaProfile {
public:
	QString name;
	QString token;
};

class SoapRequest : QObject {
	Q_OBJECT
public:
	SoapRequest();
	~SoapRequest();

	bool sendRequest(QString &result);
public slots:
	void authRequired(QNetworkReply *reply, QAuthenticator *authenticator);

public:
	QString username, password, host;
	QString body, action;
	QList<QString> XMLNs;
	QNetworkAccessManager *networkManager;

private:
	QString createRequest();
	QString createUserToken();
};

class OnvifPTZService {
private:
	QList<QString> ptzNameSpace;

public:
	OnvifPTZService();
	bool ContinuousMove(QString host, QString username, QString password,
			    QString profile, double x, double y, double z);
	bool AbsoluteMove(QString host, QString username, QString password,
			  QString profile, int x, int y, int z);
	bool RelativeMove(QString host, QString username, QString password,
			  QString profile, int x, int y, int z);
	bool Stop(QString host, QString username, QString password,
		  QString profile);
	bool GoToHomePosition(QString host, QString username, QString password,
			      QString profile);
	bool SetPreset(QString host, QString username, QString password,
		       QString profile, QString preset, int p);
	bool GotoPreset(QString host, QString username, QString password,
			QString profile, QString preset);
	bool RemovePreset(QString host, QString username, QString password,
			  QString profile, QString preset);
	QMap<int, QString> GetPresets(QString host, QString username,
				      QString password, QString profile);
};

class OnvifDeviceCapabilities {
public:
	QString mediaXAddr;
	QString ptzXAddr;
};

class OnvifDeviceService {
private:
	QList<QString> deviceNameSpace;

public:
	OnvifDeviceService();
	OnvifDeviceCapabilities GetCapabilities(QString deviceXAddress,
						QString username,
						QString password);
};

class OnvifMediaService {
private:
	QList<QString> mediaNameSpace;

public:
	OnvifMediaService();

	QList<MediaProfile> GetProfiles(QString mediaXAddress, QString username,
					QString password);
};

class PTZOnvif : public PTZDevice {
	Q_OBJECT

private:
	QString host;
	int port;
	QString username;
	QString password;

	QString m_PTZAddress{""};
	MediaProfile m_selectedMedia;

private slots:
	void connectCamera();

public:
	PTZOnvif(OBSData config);

	void set_config(OBSData ptz_data);
	OBSData get_config();

	obs_properties_t *get_obs_properties();

	void pantilt(double pan, double tilt);
	void pantilt_rel(int pan, int tilt);
	void pantilt_abs(int pan, int tilt);
	void pantilt_home();
	void zoom(double speed);
	void zoom_abs(int pos);
	void memory_reset(int i);
	void memory_set(int i);
	void memory_recall(int i);
};
