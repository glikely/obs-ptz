/* UART wrapper class
 *
 * Copyright 2020-2022 Grant Likely <grant.likely@secretlab.ca>
 *
 * SPDX-License-Identifier: GPLv2+
 */
#pragma once

#include <QObject>
#include <obs.hpp>
#include <QSerialPort>

/*
 * Protocol UART wrapper abstract base class
 */
class PTZUARTWrapper : public QObject {
	Q_OBJECT

protected:
	QString port_name;
	QSerialPort uart;
	QByteArray rxbuffer;

signals:
	void receive(const QByteArray &packet);
	void reset();

public:
	PTZUARTWrapper(QString &port_name);
	virtual bool open();
	void close();
	void setBaudRate(int baudRate);
	int baudRate() const;
	virtual void setConfig(OBSData config);
	virtual void save(OBSData config) const;
	virtual void addOBSProperties(obs_properties_t *props);
	virtual void send(const QByteArray &packet);
	virtual void receiveBytes(const QByteArray &bytes) = 0;
	QString portName() const { return port_name; }

public slots:
	void poll();
};
