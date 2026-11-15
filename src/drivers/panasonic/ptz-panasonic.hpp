/* Pan Tilt Zoom Panasonic HTTP CGI Implementation
 *
 * Copyright 2026 Grant Likely <grant.likely@secretlab.ca>
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#pragma once

#include <QObject>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QSet>
#include <QString>
#include <QTimer>
#include "ptz-device.hpp"

class PTZPanasonic : public PTZDevice {
	Q_OBJECT

private:
	QString host; /* configured; empty to follow the source's */
	QString m_sourceHost;
	QString effectiveHost() const { return host.isEmpty() ? m_sourceHost : host; }
	int port = 80;

	QNetworkAccessManager m_networkManager;
	QNetworkAccessManager m_statusNetworkManager;
	QTimer m_statusTimer;
	QTimer m_pollSoonTimer;

	int m_ptInFlight = 0;
	int m_zInFlight = 0;
	int m_fInFlight = 0;
	int m_lastPp = -1;
	int m_lastTt = -1;
	int m_lastZz = -1;
	int m_lastFf = -1;
	bool m_camdataInFlight = false;
	QSet<QString> m_drivePending;
	int m_consecutiveFailures = 0;

	void sendDrive();
	void sendPtzCommand(const QString &cmd, const char *category = nullptr);
	void sendCamCommand(const QString &cmd);
	void pollStatus();
	void pollStatusSoon();
	void parseCamdata(const QString &body);

private slots:
	void requestFinished(QNetworkReply *reply);
	void statusRequestFinished(QNetworkReply *reply);

public:
	PTZPanasonic(OBSData config, obs_source_t *source = nullptr);
	~PTZPanasonic() override;

	static void defaults(obs_data_t *config);
	void update(OBSData ptz_data) override;
	void onParentHostChanged(const QString &newHost) override;
	void save(OBSData ptz_data) const override;
	void saveDefaults(obs_data_t *settings) const override;
	obs_properties_t *get_obs_properties() override;

	Features features() const override;
	bool slotPresets() const override { return true; }

	void moveDrive(OBSData speeds) override;
	void moveAbs(OBSData positions) override;
	void pantilt_home() override;
	void set_autofocus(bool enabled) override;
	void focus_onetouch() override;
	void memory_set(int i) override;
	void memory_recall(int i) override;
	void memory_reset(int i) override;
	void requestState(OBSData requested) override;
};

void ptz_panasonic_register_filter();
