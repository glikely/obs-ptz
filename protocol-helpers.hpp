/* Pan Tilt Zoom camera instance
 *
 * Copyright 2020 Grant Likely <grant.likely@secretlab.ca>
 *
 * SPDX-License-Identifier: GPLv2
 */
#pragma once

#include <QObject>
#include <QTimer>
#include <QSerialPort>
#include <obs.hpp>

/*
 * Datagram field encoding helpers
 */
class datagram_field {
public:
	const char *name;
	int offset;
	datagram_field(const char *name, int offset) : name(name), offset(offset) { }
	virtual void encode(QByteArray &msg, int val) = 0;
	virtual int decode(QByteArray &msg) = 0;
};

class bool_field : public datagram_field {
public:
	const unsigned int mask;
	bool_field(const char *name, unsigned offset, unsigned int mask) :
			datagram_field(name, offset), mask(mask) { }
	void encode(QByteArray &msg, int val) {
		unsigned int encoded = 0;
		unsigned int current_bit = 0;
		if (msg.size() < offset)
			return;
		msg[offset] = (msg[offset] & ~mask) | (val ? mask : 0);
	}
	int decode(QByteArray &msg) {
		if (msg.size() < offset)
			return 0;
		return (msg[offset] & mask) != 0;
	}
};

class int_field : public datagram_field {
public:
	const unsigned int mask;
	int size, extend_mask = 0;
	int_field(const char *name, unsigned offset, unsigned int mask, bool signextend = false) :
			datagram_field(name, offset), mask(mask)
	{
		// Calculate number of bytes in the value
		unsigned int wm = mask;
		size = 0;
		while (wm) {
			wm >>= 8;
			size++;
		}

		// Calculate the mask for sign extending
		if (signextend) {
			int bitcount = 0;
			wm = mask;
			while (wm) {
				wm &= wm - 1;
				bitcount++;
			}
			extend_mask = 1U << (bitcount - 1);
		}
	}
	void encode(QByteArray &msg, int val) {
		unsigned int encoded = 0;
		unsigned int current_bit = 0;
		if (msg.size() < offset + size)
			return;
		for (unsigned int wm = mask; wm; wm = wm >> 1, current_bit++) {
			if (wm & 1) {
				encoded |= (val & 1) << current_bit;
				val = val >> 1;
			}
		}
		for (int i = 0; i < size; i++)
			msg[offset + i] = (encoded >> (size - i - 1) * 8) & 0xff;
	}
	int decode(QByteArray &msg) {
		unsigned int encoded = 0;
		unsigned int val = 0;
		unsigned int current_bit = 0;
		if (msg.size() < offset + size)
			return 0;
		for (int i = 0; i < size; i++)
			encoded = encoded << 8 | msg[offset+i];
		for (unsigned int wm = mask; wm; wm >>= 1, encoded >>= 1) {
			if (wm & 1) {
				val |= (encoded & 1) << current_bit;
				current_bit++;
			}
		}
		val = (val ^ extend_mask) - extend_mask;
		return val;
	}
};

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
	int baudRate();
	virtual void setConfig(OBSData config);
	virtual OBSData getConfig();
	virtual void addOBSProperties(obs_properties_t *props);
	virtual void send(const QByteArray &packet);
	virtual void receiveBytes(const QByteArray &bytes) = 0;
	QString portName() { return port_name; }

public slots:
	void poll();
};
