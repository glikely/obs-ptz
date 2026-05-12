/* ONVIF WS-Discovery probe and selection dialog
 *
 * Copyright 2026 Jonatã Bolzan Loss <jonata@jonata.org>
 *
 * SPDX-License-Identifier: GPLv2
 */
#pragma once

#include <QDialog>
#include <QNetworkAccessManager>
#include <QObject>
#include <QString>
#include <QStringList>
#include <QUdpSocket>
#include <QTimer>
#include <QSet>
#include <QList>
#include <QMetaType>

class QNetworkReply;
class QTableWidget;
class QLineEdit;
class QLabel;
class QPushButton;
class QPlainTextEdit;

struct OnvifCameraInfo {
	QString uuid;
	QString xaddr;
	QString host;
	int port = 80;
	QString manufacturer;
	QString model;
	QString name;
	QString location;
	QStringList scopes;
	QStringList types;
};
Q_DECLARE_METATYPE(OnvifCameraInfo)

class OnvifDiscovery : public QObject {
	Q_OBJECT

public:
	explicit OnvifDiscovery(QObject *parent = nullptr);

	void start(int timeoutMs = 4000);
	void stop();
	bool isRunning() const { return m_running; }

signals:
	void cameraFound(const OnvifCameraInfo &cam);
	void finished();
	void errorOccurred(const QString &message);

private slots:
	void onReadyRead();
	void onTimeout();

private:
	void sendProbe();
	void parseProbeMatch(const QByteArray &payload, const QHostAddress &sender);

	QUdpSocket m_socket;
	QTimer m_timer;
	QString m_messageId;
	QSet<QString> m_seenUuids;
	bool m_running = false;
};

/**
 * Fetches RTSP stream URIs for a camera by walking GetCapabilities →
 * GetProfiles → GetStreamUri. All requests are async; results are reported
 * via streamsReady() (success) or error() (failure).
 */
class OnvifMediaProbe : public QObject {
	Q_OBJECT

public:
	struct StreamInfo {
		QString profileToken;
		QString profileName;
		QString uri;
	};

	explicit OnvifMediaProbe(QObject *parent = nullptr);

	void fetch(const QString &host, int port, const QString &username, const QString &password);
	void cancel();

signals:
	void streamsReady(const QList<OnvifMediaProbe::StreamInfo> &streams);
	void error(const QString &message);

private slots:
	void onReplyFinished(QNetworkReply *reply);

private:
	enum class Step { Idle, SystemDateAndTime, Capabilities, Profiles, StreamUri };

	void sendRequest(const QString &url, const QString &action, const QString &body, bool authenticated);
	void requestSystemDateAndTime();
	void requestCapabilities();
	void requestProfiles();
	void requestNextStreamUri();
	QString wsSecurityHeader() const;

	QNetworkAccessManager m_nam;
	QNetworkReply *m_currentReply = nullptr;
	Step m_step = Step::Idle;
	QString m_host;
	int m_port = 80;
	QString m_username;
	QString m_password;
	QString m_deviceXAddr;
	QString m_mediaXAddr;
	QStringList m_profileTokens;
	QStringList m_profileNames;
	int m_profileIdx = 0;
	QList<StreamInfo> m_results;
	qint64 m_timeOffsetSecs = 0;
	int m_token = 0; /* incremented to invalidate stale replies after cancel/restart */
};
Q_DECLARE_METATYPE(OnvifMediaProbe::StreamInfo)

class OnvifDiscoveryDialog : public QDialog {
	Q_OBJECT

public:
	explicit OnvifDiscoveryDialog(QWidget *parent = nullptr);

	OnvifCameraInfo selectedCamera() const { return m_selected; }
	QString selectedUsername() const;
	QString selectedPassword() const;

private slots:
	void onRescanClicked();
	void onCameraFound(const OnvifCameraInfo &cam);
	void onDiscoveryFinished();
	void onSelectionChanged();
	void onCredentialsEdited();
	void onAddManualClicked();
	void onStreamsReady(const QList<OnvifMediaProbe::StreamInfo> &streams);
	void onStreamProbeError(const QString &msg);
	void accept() override;

private:
	void rebuildDetail();
	void maybeProbeStreams();
	void appendCamera(const OnvifCameraInfo &cam);

	OnvifDiscovery *m_discovery;
	OnvifMediaProbe *m_mediaProbe;
	QTableWidget *m_table;
	QLabel *m_statusLabel;
	QPushButton *m_rescanButton;
	QPushButton *m_okButton;
	QLineEdit *m_userEdit;
	QLineEdit *m_passEdit;
	QLineEdit *m_manualHostEdit;
	QLineEdit *m_manualPortEdit;
	QPushButton *m_addManualButton;
	QPlainTextEdit *m_detailView;
	QList<OnvifCameraInfo> m_cameras;
	OnvifCameraInfo m_selected;
	QString m_streamStatus;
	QList<OnvifMediaProbe::StreamInfo> m_streams;
};
