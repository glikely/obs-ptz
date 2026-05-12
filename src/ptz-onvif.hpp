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
	void getSystemDateAndTime();
	void getCapabilities();
	void getProfiles();
	void getPresets();
	void getStatus();
	void handleResponse(QString response);
	void handleGetPresetsResponse(QDomDocument &doc);
	void handleSetPresetResponse(QDomDocument &doc);
	void handleGetCapabilitiesResponse(QDomNode node);
	void handleGetProfilesResponse(QDomNode node);
	void handleGetSystemDateAndTimeResponse(QDomNode node);
	void handleGetStatusResponse(QDomNode node);
	void ensureCapabilitiesRequested();

	QTimer m_statusTimer;
	double m_position_pan = 0.0;
	double m_position_tilt = 0.0;
	double m_position_zoom = 0.0;
	/* Consecutive request failures since the last good response. When
	 * this crosses a threshold we flip the dock indicator to red and
	 * kick a full reconnect attempt on the next timer tick. */
	int m_consecutiveFailures = 0;

	/* Slot that's waiting for its SetPresetResponse to come back with the
	 * new camera-assigned token. -1 means no SetPreset is in flight. */
	int m_pendingSetPresetSlot = -1;
	/* Local-to-camera time offset (seconds). Computed from
	 * GetSystemDateAndTime on connect. Used so WS-Security timestamps
	 * still validate against cameras whose clocks have drifted. */
	qint64 m_timeOffsetSecs = 0;
	/* Once true, we've already issued getCapabilities; suppress duplicates
	 * regardless of whether time-sync succeeded or failed. */
	bool m_capabilitiesRequested = false;

	void genericMove(QString movetype, QString property, double pan, double tilt, double zoom);
	void continuousMove(double x, double y, double z);
	void absoluteMove(double x, double y, double z);
	void relativeMove(double x, double y, double z);
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
