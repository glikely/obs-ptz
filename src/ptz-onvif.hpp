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
	QString videoSourceToken;
};

class PTZOnvif : public PTZDevice {
	Q_OBJECT

private:
	bool m_isBusy = false;
	QString host; /* configured; empty to follow the source's */
	QString m_sourceHost;
	QString effectiveHost() const { return host.isEmpty() ? m_sourceHost : host; }
	int port;
	QString username;
	QString password;
	QNetworkAccessManager m_networkManager;

	QString m_mediaXAddr{""};
	QString m_PTZAddress{""};
	QString m_imagingXAddr{""};
	MediaProfile m_selectedMedia;
	QList<MediaProfile> m_mediaProfiles;
	/* Token the user picked previously; kept across reconnects so we can
	 * re-pick the same profile after a GetProfiles refresh. */
	QString m_savedProfileToken;

	/* ONVIF-specific velocity multiplier. ONVIF velocity is normalized
	 * [-1.0, 1.0] in the spec, but many cameras accept (and respect)
	 * larger values for faster moves. Defaults to 1.0 (spec-compliant). */
	double m_speed_boost = 1.0;

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
	/* Read the position again shortly, once a move has had time to start
	 * and again to finish, rather than at the timer's slow pace */
	void pollStatusSoon();
	/* Consecutive request failures since the last good response. When
	 * this crosses a threshold we flip the dock indicator to red and
	 * kick a full reconnect attempt on the next timer tick. */
	int m_consecutiveFailures = 0;

	/* The presets asked for and not made yet, in order, which the camera makes one at a
	 * time: its answer to SetPreset has the token of the preset it made, and nothing
	 * to say which request it was for. The first is the one in flight. */
	struct CreateRequest {
		QString request;
		QString name;
	};
	QList<CreateRequest> m_creates;
	bool m_createInFlight = false;
	QTimer m_createTimer;
	void startNextCreate();
	void completeCreate(const QString &token);
	void presetRequest(const QString &operation, const QString &token, const QString &name = QString());
	/* Local-to-camera time offset (seconds). Computed from
	 * GetSystemDateAndTime on connect. Used so WS-Security timestamps
	 * still validate against cameras whose clocks have drifted. */
	qint64 m_timeOffsetSecs = 0;
	/* Once true, we've already issued getCapabilities; suppress duplicates
	 * regardless of whether time-sync succeeded or failed. */
	bool m_capabilitiesRequested = false;

	/* An axis left out (panTilt or zoom false) isn't moved */
	void genericMove(QString movetype, QString property, double pan, double tilt, double zoom, bool panTilt = true,
			 bool zoomAxis = true);
	void continuousMove(double x, double y, double z);
	void absoluteMove(double x, double y, double z, bool panTilt = true, bool zoomAxis = true);
	void relativeMove(double x, double y, double z);
	void stop();
	void goToHomePosition();

	void imagingFocusMove(double speed);
	void imagingFocusStop();
	void imagingSetAutoFocus(bool autoFocus);
	void imagingSetWhiteBalance(const QString &mode);
	void applyImagingIfPending();

	/* Imaging settings the user picked. Empty string means "don't touch". */
	QString m_wbMode;
	bool m_imagingDirty = false;

private slots:
	void connectCamera();
	void authRequired(QNetworkReply *reply, QAuthenticator *authenticator);
	void requestFinished(QNetworkReply *reply);

public:
	PTZOnvif(OBSData config, obs_source_t *source = nullptr);

	static void defaults(obs_data_t *config);
	void update(OBSData ptz_data) override;
	void onParentHostChanged(const QString &newHost) override;
	void save(OBSData ptz_data) const override;
	void persistState(obs_data_t *settings) const override;
	void saveDefaults(obs_data_t *settings) const override;

	obs_properties_t *get_obs_properties() override;

	void do_update() override;
	void pantilt_rel(double pan, double tilt) override;
	void pantilt_abs(double pan, double tilt) override;
	void pantilt_home() override;
	void pantilt_set_home() override;
	Features features() const override;
	void zoom_abs(double pos) override;
	void set_autofocus(bool enabled) override;
	CameraPresets cameraPresets() const override;
	bool cameraPresetCreate(const QString &request, const QString &name) override;
	void cameraPresetSave(const QString &key) override;
	void cameraPresetRecall(const QString &key) override;
	void cameraPresetDelete(const QString &key) override;
	void cameraPresetRename(const QString &key, const QString &name) override;
	void cameraPresetRefresh() override;
};

void ptz_onvif_register_filter();
