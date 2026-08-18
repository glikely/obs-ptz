/* UART wrapper class
 *
 * Copyright 2020-2022 Grant Likely <grant.likely@secretlab.ca>
 *
 * SPDX-License-Identifier: GPLv2
 */
#pragma once

#include <QObject>
#include <QThread>
#include <QTimer>
#include <obs.hpp>
#include <atomic>
#include <serial_cpp/serial.h>

/*
 * Protocol UART wrapper abstract base class
 */
class PTZUARTWrapper : public QObject {
	Q_OBJECT

protected:
	QByteArray rxbuffer;

private:
	serial_cpp::Serial uart;
	QThread *reader_thread = nullptr;
	std::atomic<bool> stop_reader = false;
	QTimer reconnect_timer;
	/* Bumped on every open(); lets a reader thread's queued disconnect
	 * request (see open()) recognize a close()/open() cycle already ran
	 * again before that queued call was processed, and skip closing the
	 * new session. Only ever touched from this object's own thread. */
	uint64_t generation = 0;
	/* Set on the first failed open() attempt in a row, so reconnect_timer
	 * retrying every 2s against a port that's simply not there yet - the
	 * common case, e.g. before the camera is ever plugged in - logs once
	 * instead of spamming. Cleared as soon as open() succeeds, so a real
	 * failure right after that still logs. */
	bool open_failure_logged = false;

	/* Stops the reader thread and closes the port, with no side effects
	 * beyond that - unlike reconnect(), does not arm reconnect_timer, so
	 * this is what the destructor uses to avoid starting a retry timer on
	 * an object that's being torn down. */
	void close();

signals:
	void receive(const QByteArray &packet);
	void reset();

public:
	PTZUARTWrapper(QString &port_name);
	~PTZUARTWrapper();
	virtual bool open();
	void reconnect();
	void setBaudRate(int baudRate);
	int baudRate() const;
	virtual void setConfig(OBSData config);
	virtual void save(OBSData config) const;
	virtual void addOBSProperties(obs_properties_t *props);
	virtual void send(const QByteArray &packet);
	virtual void receiveBytes(const QByteArray &bytes) = 0;
	QString portName() const { return QString::fromStdString(uart.getPort()); }
};
