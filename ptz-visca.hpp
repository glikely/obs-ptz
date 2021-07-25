/* Pan Tilt Zoom camera instance
 *
 * Copyright 2020 Grant Likely <grant.likely@secretlab.ca>
 *
 * SPDX-License-Identifier: GPLv2
 */
#pragma once

#include <QObject>
#include <QTimer>
#include <QUdpSocket>
#include "protocol-helpers.hpp"
#include "ptz-device.hpp"
#include "protocol-helpers.hpp"

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

	virtual void send_immediate(QByteArray &msg) = 0;
	void send(PTZCmd cmd);
	void send(PTZCmd cmd, QList<int> args);
	void send_pending();
	void timeout();

protected slots:
	void receive(const QByteArray &msg);

public:
	PTZVisca(std::string type);

	virtual void set_config(OBSData ptz_data) = 0;
	virtual OBSData get_config() = 0;

	void cmd_get_camera_info();
	void pantilt(double pan, double tilt);
	void pantilt_rel(int pan, int tilt);
	void pantilt_abs(int pan, int tilt);
	void pantilt_stop();
	void pantilt_home();
	void zoom_stop();
	void zoom_tele(double speed);
	void zoom_wide(double speed);
	void zoom_abs(int pos);
	void set_autofocus(bool enabled);
	void focus_stop();
	void focus_near(double speed);
	void focus_far(double speed);
	void focus_onetouch();
	void memory_reset(int i);
	void memory_set(int i);
	void memory_recall(int i);
};

/*
 * VISCA over Serial UART classes
 */
class ViscaUART : public PTZUARTWrapper {
	Q_OBJECT

private:
	/* Global lookup table of UART instances, used to eliminate duplicates */
	static std::map<QString, ViscaUART*> interfaces;

	int camera_count;

public:
	ViscaUART(QString &port_name);
	bool open();
	void close();
	void receive_datagram(const QByteArray &packet);
	void receiveBytes(const QByteArray &packet);

	static ViscaUART *get_interface(QString port_name);
};

class PTZViscaSerial : public PTZVisca {
	Q_OBJECT

private:
	ViscaUART *iface;
	void attach_interface(ViscaUART *iface);

protected:
	void send_immediate(QByteArray &msg);
	void reset();

public:
	PTZViscaSerial(OBSData config);
	~PTZViscaSerial();

	void set_config(OBSData ptz_data);
	OBSData get_config();
	obs_properties_t *get_obs_properties();
};

/*
 * VISCA over IP classes
 */
class ViscaUDPSocket : public QObject {
	Q_OBJECT

private:
	/* Global lookup table of UART instances, used to eliminate duplicates */
	static std::map<int, ViscaUDPSocket*> interfaces;

	int visca_port;
	QUdpSocket visca_socket;

signals:
	void receive(const QByteArray &packet);
	void reset();

public:
	ViscaUDPSocket(int port = 52381);
	void receive_datagram(QNetworkDatagram &datagram);
	void send(QHostAddress ip_address, const QByteArray &packet);
	int port() { return visca_port; }

	static ViscaUDPSocket *get_interface(int port);

public slots:
	void poll();
};

class PTZViscaOverIP : public PTZVisca {
	Q_OBJECT

private:
	int sequence;
	QHostAddress ip_address;
	ViscaUDPSocket *iface;
	void attach_interface(ViscaUDPSocket *iface);

protected:
	void send_immediate(QByteArray &msg);
	void reset();

public:
	PTZViscaOverIP(OBSData config);
	~PTZViscaOverIP();

	void set_config(OBSData ptz_data);
	OBSData get_config();
	obs_properties_t *get_obs_properties();
};
