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
#include "protocol-helpers.hpp"

#define VISCA_RESPONSE_ADDRESS   0x30
#define VISCA_RESPONSE_ACK       0x40
#define VISCA_RESPONSE_COMPLETED 0x50
#define VISCA_RESPONSE_ERROR     0x60
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

protected:
	unsigned int address;
	QList<PTZCmd> pending_cmds;
	bool active_cmd[8];
	QTimer timeout_timer;

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
	void pantilt(double pan, double tilt);
	void pantilt_rel(int pan, int tilt);
	void pantilt_abs(int pan, int tilt);
	void pantilt_home();
	void zoom(double speed);
	void zoom_abs(int pos);
	void set_autofocus(bool enabled);
	void focus(double speed);
	void focus_onetouch();
	void memory_reset(int i);
	void memory_set(int i);
	void memory_recall(int i);
};
