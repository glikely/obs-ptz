/* Pan Tilt Zoom Panasonic HTTP CGI Implementation
 *
 * Copyright 2026 Grant Likely <grant.likely@secretlab.ca>
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include <QNetworkRequest>
#include <QUrl>
#include <qt-wrappers.hpp>
#include "panasonic-protocol.hpp"
#include "ptz-panasonic.hpp"
#include "ptz.h"

using namespace panasonic_protocol;

PTZPanasonic::PTZPanasonic(OBSData config, obs_source_t *source) : PTZDevice(config, source)
{
	type = "panasonic";
	m_networkManager.setAutoDeleteReplies(true);
	connect(&m_networkManager, &QNetworkAccessManager::finished, this, &PTZPanasonic::requestFinished);

	m_statusTimer.setInterval(1500);
	connect(&m_statusTimer, &QTimer::timeout, this, &PTZPanasonic::pollStatus);
	m_statusTimer.start();

	update(config);
}

PTZPanasonic::~PTZPanasonic()
{
	m_statusTimer.stop();
}

void PTZPanasonic::sendPtzCommand(const QString &cmd, const char *category)
{
	const QString h = effectiveHost();
	if (h.isEmpty())
		return;
	QString url = QStringLiteral("http://%1:%2/cgi-bin/aw_ptz?cmd=%23%3&res=1").arg(h).arg(port).arg(cmd);
	QNetworkRequest request{QUrl(url)};
	request.setTransferTimeout(5000);
	QNetworkReply *reply = m_networkManager.get(request);
	if (category)
		reply->setProperty("ptz_category", QString::fromLatin1(category));
}

void PTZPanasonic::sendCamCommand(const QString &cmd)
{
	const QString h = effectiveHost();
	if (h.isEmpty())
		return;
	QString url = QStringLiteral("http://%1:%2/cgi-bin/aw_cam?cmd=%3&res=1").arg(h).arg(port).arg(cmd);
	QNetworkRequest request{QUrl(url)};
	request.setTransferTimeout(5000);
	m_networkManager.get(request);
}

void PTZPanasonic::pollStatus()
{
	const QString h = effectiveHost();
	if (h.isEmpty())
		return;
	QString url = QStringLiteral("http://%1:%2/live/camdata.html").arg(h).arg(port);
	QNetworkRequest request{QUrl(url)};
	request.setTransferTimeout(5000);
	QNetworkReply *reply = m_networkManager.get(request);
	reply->setProperty("ptz_category", QStringLiteral("camdata"));
}

void PTZPanasonic::pollStatusSoon()
{
	for (int delay_ms : {400, 1500}) {
		QTimer::singleShot(delay_ms, this, [this]() {
			if (isConnected())
				pollStatus();
		});
	}
}

void PTZPanasonic::requestFinished(QNetworkReply *reply)
{
	QString category = reply->property("ptz_category").toString();
	if (category == QStringLiteral("pt"))
		m_ptInFlight = false;
	else if (category == QStringLiteral("z"))
		m_zInFlight = false;
	else if (category == QStringLiteral("f"))
		m_fInFlight = false;

	if (reply->error() != QNetworkReply::NoError) {
		ptz_debug("HTTP error (%s): %s", QT_TO_UTF8(category), QT_TO_UTF8(reply->errorString()));
		++m_consecutiveFailures;
		if (m_consecutiveFailures >= 3 && isConnected())
			setConnected(false);
	} else {
		m_consecutiveFailures = 0;
		if (!isConnected())
			setConnected(true);
		QByteArray data = reply->readAll();
		if (category == QStringLiteral("camdata"))
			parseCamdata(QString::fromUtf8(data));
	}

	sendDrive();
}

void PTZPanasonic::parseCamdata(const QString &body)
{
	const CamdataUpdate parsed = parseCamdataBody(body.toStdString());
	bool changed = false;

	if (parsed.pan)
		changed |= setPosition("pan", *parsed.pan);
	if (parsed.tilt)
		changed |= setPosition("tilt", *parsed.tilt);
	if (parsed.zoom)
		changed |= setPosition("zoom", *parsed.zoom);
	if (parsed.focus)
		changed |= setPosition("focus", *parsed.focus);
	if (parsed.focusAfEnabled) {
		bool af = *parsed.focusAfEnabled;
		if (!obs_data_has_user_value(state, "focus_af_enabled") ||
		    obs_data_get_bool(state, "focus_af_enabled") != af) {
			obs_data_set_bool(state, "focus_af_enabled", af);
			obs_data_set_bool(stateChanged, "focus_af_enabled", af);
			changed = true;
		}
	}
	if (parsed.powerOn) {
		bool powerOn = *parsed.powerOn;
		if (!obs_data_has_user_value(state, "power_on") ||
		    obs_data_get_bool(state, "power_on") != powerOn) {
			obs_data_set_bool(state, "power_on", powerOn);
			obs_data_set_bool(stateChanged, "power_on", powerOn);
			changed = true;
		}
	}

	if (changed)
		notifyStateChanged();
}

void PTZPanasonic::moveDrive(OBSData speeds)
{
	for (obs_data_item_t *item = obs_data_first(speeds); item; obs_data_item_next(&item))
		m_drivePending += QString::fromUtf8(obs_data_item_get_name(item));
	sendDrive();
}

void PTZPanasonic::sendDrive()
{
	const double pan_speed = speed("pan");
	const double tilt_speed = speed("tilt");
	const double zoom_speed = speed("zoom");
	const double focus_speed = speed("focus");

	const bool moving = pan_speed != 0.0 || tilt_speed != 0.0 || zoom_speed != 0.0 || focus_speed != 0.0;
	const int interval = moving ? 500 : 1500;
	if (m_statusTimer.interval() != interval) {
		m_statusTimer.setInterval(interval);
		if (!moving)
			pollStatusSoon();
	}

	if (m_drivePending.contains(QStringLiteral("pan")) || m_drivePending.contains(QStringLiteral("tilt"))) {
		int pp = speedToPanasonic(pan_speed);
		int tt = speedToPanasonic(tilt_speed);
		bool isStop = (pp == 50 && tt == 50);
		if (isStop || !m_ptInFlight) {
			m_drivePending.remove(QStringLiteral("pan"));
			m_drivePending.remove(QStringLiteral("tilt"));
			m_ptInFlight = true;
			sendPtzCommand(QString::asprintf("PTS%02d%02d", pp, tt), "pt");
		}
	}

	if (m_drivePending.contains(QStringLiteral("zoom"))) {
		int zz = speedToPanasonic(zoom_speed);
		bool isStop = (zz == 50);
		if (isStop || !m_zInFlight) {
			m_drivePending.remove(QStringLiteral("zoom"));
			m_zInFlight = true;
			sendPtzCommand(QString::asprintf("Z%02d", zz), "z");
		}
	}

	if (m_drivePending.contains(QStringLiteral("focus"))) {
		int ff = speedToPanasonic(focus_speed);
		bool isStop = (ff == 50);
		if (isStop || !m_fInFlight) {
			m_drivePending.remove(QStringLiteral("focus"));
			m_fInFlight = true;
			sendPtzCommand(QString::asprintf("F%02d", ff), "f");
		}
	}
}

void PTZPanasonic::moveAbs(OBSData positions)
{
	if (obs_data_has_user_value(positions, "pan") || obs_data_has_user_value(positions, "tilt")) {
		uint16_t p = normToPanTiltHex(obs_data_get_double(positions, "pan"));
		uint16_t t = normToPanTiltHex(obs_data_get_double(positions, "tilt"));
		sendPtzCommand(QString::asprintf("APC%04X%04X", p, t));
	}
	if (obs_data_has_user_value(positions, "zoom")) {
		uint16_t z = normToZoomFocusHex(obs_data_get_double(positions, "zoom"));
		sendPtzCommand(QString::asprintf("AXZ%03X", z));
	}
	if (obs_data_has_user_value(positions, "focus")) {
		uint16_t f = normToZoomFocusHex(obs_data_get_double(positions, "focus"));
		sendPtzCommand(QString::asprintf("AXF%03X", f));
	}
	pollStatusSoon();
}

void PTZPanasonic::pantilt_home()
{
	sendPtzCommand(QStringLiteral("APC80008000"));
	pollStatusSoon();
}

void PTZPanasonic::set_autofocus(bool enabled)
{
	sendPtzCommand(enabled ? QStringLiteral("D11") : QStringLiteral("D10"));
	pollStatusSoon();
}

void PTZPanasonic::focus_onetouch()
{
	sendCamCommand(QStringLiteral("OSE:69:1"));
	pollStatusSoon();
}

void PTZPanasonic::memory_recall(int i)
{
	if (i < 0 || i > 99)
		return;
	sendPtzCommand(QString::asprintf("R%02d", i));
	pollStatusSoon();
}

void PTZPanasonic::memory_set(int i)
{
	if (i < 0 || i > 99)
		return;
	sendPtzCommand(QString::asprintf("M%02d", i));
}

void PTZPanasonic::memory_reset(int i)
{
	if (i < 0 || i > 99)
		return;
	sendPtzCommand(QString::asprintf("C%02d", i));
}

void PTZPanasonic::requestState(OBSData requested)
{
	PTZDevice::requestState(requested);
	if (obs_data_has_user_value(requested, "power_on")) {
		bool on = obs_data_get_bool(requested, "power_on");
		sendPtzCommand(on ? QStringLiteral("O1") : QStringLiteral("O0"));
		pollStatusSoon();
	}
}

PTZDevice::Features PTZPanasonic::features() const
{
	return Pan | Tilt | Zoom | Focus | PanAbs | TiltAbs | ZoomAbs | FocusAbs | Home | AutoFocus | FocusOneTouch |
	       Presets | Power;
}

void PTZPanasonic::defaults(obs_data_t *config)
{
	PTZDevice::defaults(config);
	obs_data_set_default_int(config, "preset_max", 100);
	obs_data_set_default_int(config, "port", 80);
}

void PTZPanasonic::update(OBSData config)
{
	PTZDevice::update(config);
	host = obs_data_get_string(config, "host");
	m_sourceHost = parentSourceHost();
	port = (int)obs_data_get_int(config, "port");
	if (port <= 0)
		port = 80;
	m_consecutiveFailures = 0;
	pollStatus();
}

void PTZPanasonic::onParentHostChanged(const QString &newHost)
{
	if (newHost == m_sourceHost)
		return;
	m_sourceHost = newHost;
	if (host.isEmpty())
		pollStatus();
}

void PTZPanasonic::save(OBSData config) const
{
	PTZDevice::save(config);
	obs_data_set_string(config, "host", QT_TO_UTF8(host));
	obs_data_set_int(config, "port", port);
}

void PTZPanasonic::saveDefaults(obs_data_t *settings) const
{
	obs_data_set_default_string(settings, "host:placeholder", QT_TO_UTF8(m_sourceHost));
}

obs_properties_t *PTZPanasonic::get_obs_properties()
{
	obs_properties_t *ptz_props = PTZDevice::get_obs_properties();
	obs_property_t *p = obs_properties_get(ptz_props, "interface");
	obs_properties_t *config = obs_property_group_content(p);
	obs_property_set_description(p, "Panasonic (HTTP)");
	obs_properties_add_text(config, "host", obs_module_text("PTZ.Device.Hostname"), OBS_TEXT_DEFAULT);
	obs_properties_add_int(config, "port", obs_module_text("PTZ.Device.TCPPort"), 1, 65535, 1);
	return ptz_props;
}

void ptz_panasonic_register_filter()
{
	struct obs_source_info info = {};
	info.id = "ca.secretlab.obs-ptz.panasonic";
	info.type = OBS_SOURCE_TYPE_FILTER;
	info.output_flags = OBS_SOURCE_VIDEO;
	info.get_name = [](void *) -> const char * {
		return "Panasonic (HTTP) PTZ Control";
	};
	info.create = [](obs_data_t *, obs_source_t *source) -> void * {
		return ptz_filter_create(source, [source](OBSData config) -> PTZDevice * {
			return new PTZPanasonic(config, source);
		});
	};
	info.destroy = ptz_filter_destroy;
	info.get_defaults = [](obs_data_t *settings) {
		ptz_filter_defaults_mark(settings);
		obs_data_set_default_string(settings, "type", "panasonic");
		PTZPanasonic::defaults(settings);
	};
	info.get_properties = ptz_filter_get_properties;
	info.update = ptz_filter_update;
	info.save = ptz_filter_save;
	info.filter_remove = ptz_filter_remove;
	info.icon_type = OBS_ICON_TYPE_CAMERA;
	info.filter_add = ptz_filter_add;
	obs_register_source(&info);
}

