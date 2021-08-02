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
#include <QUdpSocket>
#include "ptz-device.hpp"

class visca_encoding {
public:
	const char *name;
	int offset;
	visca_encoding(const char *name, int offset) : name(name), offset(offset) { }
	virtual void encode(QByteArray &data, int val) = 0;
	virtual int decode(QByteArray &data) = 0;
};

class int_field : public visca_encoding {
public:
	const unsigned int mask;
	int size;
	int_field(const char *name, unsigned offset, unsigned int mask) :
			visca_encoding(name, offset), mask(mask)
	{
		size = 0;
		unsigned int wm = mask;
		while (wm) {
			wm >>= 8;
			size++;
		}
	}
	void encode(QByteArray &data, int val) {
		unsigned int encoded = 0;
		unsigned int current_bit = 0;
		if (data.size() < offset + size)
			return;
		for (unsigned int wm = mask; wm; wm = wm >> 1, current_bit++) {
			if (wm & 1) {
				encoded |= (val & 1) << current_bit;
				val = val >> 1;
			}
		}
		for (int i = 0; i < size; i++)
			data[offset + i] = (encoded >> (size - i - 1) * 8) & 0xff;
	}
	int decode(QByteArray &data) {
		unsigned int encoded = 0;
		unsigned int val = 0;
		unsigned int current_bit = 0;
		if (data.size() < offset + size)
			return 0;
		for (int i = 0; i < size; i++)
			encoded = encoded << 8 | data[offset+i];
		for (unsigned int wm = mask; wm; wm >>= 1, encoded >>= 1) {
			if (wm & 1) {
				val |= (encoded & 1) << current_bit;
				current_bit++;
			}
		}
		return val;
	}
};

class visca_u4 : public visca_encoding {
public:
	visca_u4(const char *name, int offset) : visca_encoding(name, offset) { }
	void encode(QByteArray &data, int val) {
		if (data.size() < offset + 1)
			return;
		data[offset] = data[offset] & 0xf0 | val & 0x0f;
	}
	int decode(QByteArray &data) {
		return (data.size() < offset + 1) ? 0 : data[offset] & 0xf;
	}
};
class visca_flag : public visca_encoding {
public:
	visca_flag(const char *name, int offset) : visca_encoding(name, offset) { }
	void encode(QByteArray &data, int val) {
		if (data.size() < offset + 1)
			return;
		data[offset] = val ? 0x2 : 0x3;
	}
	int decode(QByteArray &data) {
		return (data.size() < offset + 1) ? 0 : 0x02 == data[offset];
	}
};
class visca_u7 : public visca_encoding {
public:
	visca_u7(const char *name, int offset) : visca_encoding(name, offset) { }
	void encode(QByteArray &data, int val) {
		if (data.size() < offset + 1)
			return;
		data[offset] = val & 0x7f;
	}
	int decode(QByteArray &data) {
		return (data.size() < offset + 1) ? 0 : data[offset] & 0x7f;
	}
};
class visca_s7 : public visca_encoding {
public:
	visca_s7(const char *name, int offset) : visca_encoding(name, offset) { }
	virtual void encode(QByteArray &data, int val) {
		if (data.size() < offset + 3)
			return;
		data[offset] = abs(val) & 0x7f;
		data[offset+2] = 3;
		if (val)
			data[offset+2] = val < 0 ? 1 : 2;

	}
	virtual int decode(QByteArray &data) {
		return (data.size() < offset + 3) ? 0 : data[offset] & 0x7f;
	}
};
class visca_u8 : public visca_encoding {
public:
	visca_u8(const char *name, int offset) : visca_encoding(name, offset) { }
	void encode(QByteArray &data, int val) {
		if (data.size() < offset + 2)
			return;
		data[offset]   = (val >> 4) & 0x0f;
		data[offset+1] = val & 0x0f;
	}
	int decode(QByteArray &data) {
		if (data.size() < offset + 2)
			return 0;
		int16_t val = (data[offset] & 0xf) << 4 |
			      (data[offset+1] & 0xf);
		return val;
	}
};
/* 15 bit value encoded into two bytes. Protocol encoding forces bit 15 & 7 to zero */
class visca_u15 : public visca_encoding {
public:
	visca_u15(const char *name, int offset) : visca_encoding(name, offset) { }
	void encode(QByteArray &data, int val) {
		if (data.size() < offset + 2)
			return;
		data[offset] = (val >> 8) & 0x7f;
		data[offset+1] = val & 0x7f;
	}
	int decode(QByteArray &data) {
		if (data.size() < offset + 2)
			return 0;
		uint16_t val = (data[offset] & 0x7f) << 8 |
			       (data[offset+1] & 0x7f);
		return val;
	}
};
class visca_s16 : public visca_encoding {
public:
	visca_s16(const char *name, int offset) : visca_encoding(name, offset) { }
	void encode(QByteArray &data, int val) {
		if (data.size() < offset + 4)
			return;
		data[offset] = (val >> 12) & 0x0f;
		data[offset+1] = (val >> 8) & 0x0f;
		data[offset+2] = (val >> 4) & 0x0f;
		data[offset+3] = val & 0x0f;
	}
	int decode(QByteArray &data) {
		if (data.size() < offset + 4)
			return 0;
		int16_t val = (data[offset] & 0xf) << 12 |
			      (data[offset+1] & 0xf) << 8 |
			      (data[offset+2] & 0xf) << 4 |
			      (data[offset+3] & 0xf);
		return val;
	}
};
class visca_u16 : public visca_encoding {
public:
	visca_u16(const char *name, int offset) : visca_encoding(name, offset) { }
	void encode(QByteArray &data, int val) {
		if (data.size() < offset + 4)
			return;
		data[offset] = (val >> 12) & 0x0f;
		data[offset+1] = (val >> 8) & 0x0f;
		data[offset+2] = (val >> 4) & 0x0f;
		data[offset+3] = val & 0x0f;
	}
	int decode(QByteArray &data) {
		if (data.size() < offset + 4)
			return 0;
		uint16_t val = (data[offset] & 0xf) << 12 |
			       (data[offset+1] & 0xf) << 8 |
			       (data[offset+2] & 0xf) << 4 |
			       (data[offset+3] & 0xf);
		return val;
	}
};

class ViscaCmd {
public:
	QByteArray cmd;
	QList<visca_encoding*> args;
	QList<visca_encoding*> results;
	ViscaCmd(const char *cmd_hex) : cmd(QByteArray::fromHex(cmd_hex)) { }
	ViscaCmd(const char *cmd_hex, QList<visca_encoding*> args) :
		cmd(QByteArray::fromHex(cmd_hex)), args(args) { }
	ViscaCmd(const char *cmd_hex, QList<visca_encoding*> args, QList<visca_encoding*> rslts) :
		cmd(QByteArray::fromHex(cmd_hex)), args(args), results(rslts) { }
	void encode(QList<int> arglist) {
		for (int i = 0; i < arglist.size() && i < args.size(); i++)
			args[i]->encode(cmd, arglist[i]);
	}
	obs_data_t *decode(QByteArray msg) {
		obs_data_t *data = obs_data_create();
		for (int i = 0; i < results.size(); i++)
			obs_data_set_int(data, results[i]->name, results[i]->decode(msg));
		return data;
	}
};
class ViscaInq : public ViscaCmd {
public:
	ViscaInq(const char *cmd_hex) : ViscaCmd(cmd_hex) { }
	ViscaInq(const char *cmd_hex, QList<visca_encoding*> rslts) : ViscaCmd(cmd_hex, {}, rslts) {}
};

/*
 * VISCA Abstract base class, used for both Serial UART and UDP implementations
 */
class PTZVisca : public PTZDevice {
	Q_OBJECT

protected:
	static const QMap<uint16_t, QString> viscaVendors;
	static const QMap<uint32_t, QString> viscaModels;
	unsigned int address;
	QList<ViscaCmd> pending_cmds;
	bool active_cmd[8];
	QTimer timeout_timer;

	virtual void send_immediate(QByteArray &msg) = 0;
	void send(ViscaCmd cmd);
	void send(ViscaCmd cmd, QList<int> args);
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
	void memory_reset(int i);
	void memory_set(int i);
	void memory_recall(int i);
};

/*
 * VISCA over Serial UART classes
 */
class ViscaUART : public QObject {
	Q_OBJECT

private:
	/* Global lookup table of UART instances, used to eliminate duplicates */
	static std::map<QString, ViscaUART*> interfaces;

	QString port_name;
	QSerialPort uart;
	QByteArray rxbuffer;
	int camera_count;

signals:
	void receive(const QByteArray &packet);
	void reset();

public:
	int baud_rate = QSerialPort::Baud9600;

	ViscaUART(QString &port_name, int baud_rate = QSerialPort::Baud9600);
	void open();
	void close();
	void send(const QByteArray &packet);
	void receive_datagram(const QByteArray &packet);
	QString portName() { return port_name; }

	static ViscaUART *get_interface(QString port_name, int baud_rate = QSerialPort::Baud9600);

public slots:
	void poll();
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
