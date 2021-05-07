/* Pan Tilt Zoom camera instance
 *
 * Copyright 2020 Grant Likely <grant.likely@secretlab.ca>
 *
 * SPDX-License-Identifier: GPLv2
 */
#pragma once

#include <QObject>
#include <QSerialPort>
#include "ptz-device.hpp"

/*
 * ViscaInterface implementing UART protocol
 */
class ViscaInterface : public QObject {
	Q_OBJECT

private:
	/* Global lookup table of UART instances, used to eliminate duplicates */
	static std::map<std::string, ViscaInterface*> interfaces;

	std::string uart_name;
	QSerialPort uart;
	QByteArray rxbuffer;
	int camera_count;

signals:
	void receive_ack(const QByteArray &packet);
	void receive_complete(const QByteArray &packet);
	void receive_error(const QByteArray &packet);

public:
	ViscaInterface(std::string &uart) : uart_name(uart) { open(); }
	void open();
	void close();
	void send(const QByteArray &packet);
	void receive(const QByteArray &packet);
	void cmd_enumerate();

	static ViscaInterface *get_interface(std::string uart);

public slots:
	void poll();
};


class PTZVisca : public PTZDevice {
	Q_OBJECT

private:
	ViscaInterface *iface;
	unsigned int address;

	void init();
	void send(QByteArray &msg);

public:
	PTZVisca(const char *uart_name, int address);
	PTZVisca(obs_data_t *config);
	~PTZVisca();

	void set_config(obs_data_t *ptz_data);
	obs_data_t * get_config();

	void cmd_clear();
	void cmd_get_camera_info();
	void pantilt(double pan, double tilt);
	void pantilt_stop();
	void pantilt_home();
	void zoom_stop();
	void zoom_tele();
	void zoom_wide();
};
