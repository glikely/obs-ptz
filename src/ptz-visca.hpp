/* Pan Tilt Zoom camera instance
 *
 * Copyright 2020 Grant Likely <grant.likely@secretlab.ca>
 *
 * SPDX-License-Identifier: GPLv2
 */
#pragma once

#include <QObject>
#include <QTimer>
#include "protocol-helpers.hpp"
#include "ptz-device.hpp"

#define VISCA_RESPONSE_ADDRESS 0x30
#define VISCA_RESPONSE_ACK 0x40
#define VISCA_RESPONSE_COMPLETED 0x50
#define VISCA_RESPONSE_ERROR 0x60
#define VISCA_PACKET_SENDER(pkt) ((unsigned)((pkt)[0] & 0x70) >> 4)

extern const PTZCmd VISCA_ENUMERATE;
extern const PTZCmd VISCA_IF_CLEAR;
extern const PTZCmd VISCA_Clear;

/*
 * VISCA Abstract base class, used for both Serial UART and UDP implementations
 */
class PTZVisca : public PTZDevice {
	Q_OBJECT

public:
	static const QMap<int, std::string> viscaVendors;
	static const QMap<int, std::string> viscaModels;
	static const QMap<QString, PTZInq> inquires;

protected:
	unsigned int timeout_retry = 0;
	unsigned int address;
	QList<PTZCmd> pending_cmds;
	std::optional<PTZCmd> active_cmd[8];
	QTimer timeout_timer;

	bool send_pantilt();
	virtual void send_immediate(const QByteArray &msg) = 0;
	void send(PTZCmd cmd);
	void send(PTZCmd cmd, QList<int> args);
	void send_pending();
	void timeout();

protected slots:
	void receive(const QByteArray &msg);

public:
	PTZVisca(OBSData config);
	obs_properties_t *get_obs_properties();

	virtual void set_config(OBSData ptz_data) = 0;
	virtual OBSData get_config() = 0;
	void set_settings(OBSData setting);

	void cmd_get_camera_info();

	void do_update();
	void pantilt_rel(double pan, double tilt);
	void pantilt_abs(double pan, double tilt);
	void pantilt_home();
	void zoom_abs(double pos);
	void set_autofocus(bool enabled);
	void focus_onetouch();
	void memory_reset(int i);
	void memory_set(int i);
	void memory_recall(int i);
};
