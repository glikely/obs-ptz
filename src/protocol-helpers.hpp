/* PTZ Protocol helper classes and functions
 *
 * Copyright 2020-2022 Grant Likely <grant.likely@secretlab.ca>
 *
 * SPDX-License-Identifier: GPLv2
 */
#pragma once

#include <QObject>
#include <QTimer>
#include <obs.hpp>

/*
 * Data Helpers
 */
OBSData variantMapToOBSData(const QVariantMap &map);
QVariantMap OBSDataToVariantMap(const OBSData data);

/*
 * Datagram field encoding helpers
 */
class datagram_field {
public:
	const char *name;
	int offset;
	datagram_field(const char *name, int offset)
		: name(name), offset(offset)
	{
	}
	virtual void encode(QByteArray &msg, int val) = 0;
	virtual bool decode(OBSData data, QByteArray &msg) = 0;
};

class bool_field : public datagram_field {
public:
	const unsigned int mask;
	bool_field(const char *name, unsigned offset, unsigned int mask)
		: datagram_field(name, offset), mask(mask)
	{
	}
	void encode(QByteArray &msg, int val);
	bool decode(OBSData data, QByteArray &msg);
};

class int_field : public datagram_field {
public:
	const unsigned int mask;
	int size, extend_mask = 0;
	int_field(const char *name, unsigned offset, unsigned int mask,
		  bool signextend = false);
	void encode(QByteArray &msg, int val);
	bool decode_int(int *val_, QByteArray &msg);
	bool decode(OBSData data, QByteArray &msg);
};

class string_lookup_field : public int_field {
public:
	const QMap<int, std::string> &lookup;
	string_lookup_field(const char *name,
			    const QMap<int, std::string> &lookuptable,
			    unsigned offset, unsigned int mask,
			    bool signextend = false)
		: int_field(name, offset, mask, signextend), lookup(lookuptable)
	{
	}
	bool decode(OBSData data, QByteArray &msg);
};

class PTZCmd {
public:
	QByteArray cmd;
	QList<datagram_field *> args;
	QList<datagram_field *> results;
	QString affects;
	PTZCmd(const char *cmd_hex, QString affects = "")
		: cmd(QByteArray::fromHex(cmd_hex)), affects(affects)
	{
	}
	PTZCmd(const char *cmd_hex, QList<datagram_field *> args,
	       QString affects = "")
		: cmd(QByteArray::fromHex(cmd_hex)),
		  args(args),
		  affects(affects)
	{
	}
	PTZCmd(const char *cmd_hex, QList<datagram_field *> args,
	       QList<datagram_field *> rslts)
		: cmd(QByteArray::fromHex(cmd_hex)), args(args), results(rslts)
	{
	}
	void encode(QList<int> arglist);
	obs_data_t *decode(QByteArray msg);
};

class PTZInq : public PTZCmd {
public:
	PTZInq() : PTZCmd("") {}
	PTZInq(const char *cmd_hex) : PTZCmd(cmd_hex) {}
	PTZInq(const char *cmd_hex, QList<datagram_field *> rslts)
		: PTZCmd(cmd_hex, {}, rslts)
	{
	}
};

extern int scale_speed(double speed, int max);
