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
#include <QtXml/QDomDocument>
#include <QXmlStreamWriter>
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

class PTZOnvif : public PTZDevice {
	Q_OBJECT

private:
	bool m_isBusy = false;
	QString host;
	int port;
	QString username;
	QString password;
	QNetworkAccessManager m_networkManager;

	QString m_mediaXAddr{""};
	QString m_PTZAddress{""};
	MediaProfile m_selectedMedia;

	// SOAP/XML helpers
	void writeHeader(QXmlStreamWriter &s, const QString action);

	void sendRequest(QString host, QString req);
	void getCapabilities();
	void getProfiles();
	void getPresets();
	void handleResponse(QString response);
	void handleGetPresetsResponse(QDomDocument &doc);
	void handleGetCapabilitiesResponse(QDomNode node);
	void handleGetProfilesResponse(QDomNode node);

	void genericMove(QString movetype, QString property, double pan, double tilt, double zoom);
	void continuousMove(double x, double y, double z);
	void absoluteMove(int x, int y, int z);
	void relativeMove(int x, int y, int z);
	void stop();
	void goToHomePosition();

private slots:
	void connectCamera();
	void authRequired(QNetworkReply *reply, QAuthenticator *authenticator);
	void requestFinished(QNetworkReply *reply);

public:
	PTZOnvif(OBSData config);
	virtual QString description();

	void set_config(OBSData ptz_data);
	OBSData get_config();

	obs_properties_t *get_obs_properties();

	void do_update();
	void pantilt_rel(double pan, double tilt);
	void pantilt_abs(double pan, double tilt);
	void pantilt_home();
	void zoom_abs(double pos);
	void memory_reset(int i);
	void memory_set(int i);
	void memory_recall(int i);
};
