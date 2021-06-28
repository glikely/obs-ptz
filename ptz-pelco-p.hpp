/* Pan Tilt Zoom camera pelco-p instance
 *
 * Copyright 2021 Luuk Verhagen <developer@verhagenluuk.nl>
 *
 * SPDX-License-Identifier: GPLv2
 */

#pragma once
#include "ptz-device.hpp"
#include <util/base.h>
#include <QObject>
#include <QSerialPort>
#include <QDebug>
#include <QStringListModel>
#include <QtGlobal>

/*
* General Serial UART class
*/
class PelcoPUART : public QObject {
	Q_OBJECT

private:
	static std::map<QString, PelcoPUART*> interfaces;

	QString port_name;
	qint32 baud_rate;
	QSerialPort uart;
	QByteArray rxbuffer;

	const int messageLength = 8;

signals:
	void receive(const QByteArray& packet);

public:

	PelcoPUART(QString& port_name, qint32 baudrate = 9600);
	void open();
	void close();
	void send(const QByteArray& packet);
	void receive_datagram(const QByteArray& packet);
	QString portName() { return port_name; }


	static PelcoPUART* get_interface(QString port_name);
	static PelcoPUART* add_interface(QString port_name, qint32 baudrate = 9600);

public slots:
	void poll();
};

class PTZPelcoP : public PTZDevice
{ 
private:
	PelcoPUART* iface;
	void attach_interface(PelcoPUART* new_iface);
	char checkSum(QByteArray &data);

protected:
	unsigned int address;
	
	void send(const QByteArray &msg);
	void send(const unsigned char data_1, const unsigned char data_2, const unsigned char data_3, const unsigned char data_4);
	void zoom_speed_set(double speed);
	void receive(const QByteArray &msg);

public:
	PTZPelcoP(OBSData data);
	~PTZPelcoP();

	void set_config(OBSData ptz_data);
	OBSData get_config();

	void pantilt(double pan, double tilt);
	void pantilt_rel(int pan, int tilt);
	void pantilt_stop();
	void pantilt_home();
	void zoom_stop();
	void zoom_tele(double speed);
	void zoom_wide(double speed);
	void memory_reset(int i);
	void memory_set(int i);
	void memory_recall(int i);
};
