/* PTZ Protocol helper classes and functions
 *
 * Copyright 2020-2022 Grant Likely <grant.likely@secretlab.ca>
 *
 * SPDX-License-Identifier: GPLv2
 */
#pragma once

#include <memory>
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
	datagram_field(const char *name, int offset) : name(name), offset(offset) {}
	virtual ~datagram_field() = default;
	virtual void encode(QByteArray &msg, int val) = 0;
	virtual bool decode(OBSData data, QByteArray &msg) = 0;
};

class bool_field : public datagram_field {
public:
	const unsigned int mask;
	bool_field(const char *name, unsigned offset, unsigned int mask) : datagram_field(name, offset), mask(mask) {}
	void encode(QByteArray &msg, int val);
	bool decode(OBSData data, QByteArray &msg);
};

class int_field : public datagram_field {
public:
	const unsigned int mask;
	int size, extend_mask = 0;
	int_field(const char *name, unsigned offset, unsigned int mask, bool signextend = false);
	void encode(QByteArray &msg, int val);
	bool decode_int(int *val_, QByteArray &msg);
	bool decode(OBSData data, QByteArray &msg);
};

class string_lookup_field : public int_field {
public:
	const QMap<int, std::string> &lookup;
	string_lookup_field(const char *name, const QMap<int, std::string> &lookuptable, unsigned offset,
			    unsigned int mask, bool signextend = false)
		: int_field(name, offset, mask, signextend),
		  lookup(lookuptable)
	{
	}
	bool decode(OBSData data, QByteArray &msg);
};

/* A command or inquiry takes ownership of the fields it is given, and its
 * copies share them */
using datagram_fields = QList<std::shared_ptr<datagram_field>>;

/* The state keys a command changes: one, or a list of them */
class affected_keys : public QStringList {
public:
	affected_keys() {}
	affected_keys(const char *key) : QStringList(QString(key)) {}
	affected_keys(std::initializer_list<QString> keys) : QStringList(keys) {}
};

class PTZCmd {
public:
	QByteArray cmd;
	datagram_fields args;
	datagram_fields results;
	QStringList affects;
	PTZCmd(const char *cmd_hex, affected_keys affects = {}) : cmd(QByteArray::fromHex(cmd_hex)), affects(affects) {}
	PTZCmd(const char *cmd_hex, QList<datagram_field *> args, affected_keys affects = {})
		: cmd(QByteArray::fromHex(cmd_hex)),
		  args(own(args)),
		  affects(affects)
	{
	}
	PTZCmd(const char *cmd_hex, QList<datagram_field *> args, QList<datagram_field *> rslts)
		: cmd(QByteArray::fromHex(cmd_hex)),
		  args(own(args)),
		  results(own(rslts))
	{
	}
	void encode(QList<int> arglist);
	obs_data_t *decode(QByteArray msg);
	/* A VISCA inquiry, as opposed to a command */
	bool isInquiry() const { return cmd.size() > 1 && cmd[1] == 0x09; }

private:
	static datagram_fields own(const QList<datagram_field *> &fields)
	{
		datagram_fields owned;
		for (auto field : fields)
			owned.append(std::shared_ptr<datagram_field>(field));
		return owned;
	}
};

class PTZInq : public PTZCmd {
public:
	PTZInq() : PTZCmd("") {}
	PTZInq(const char *cmd_hex) : PTZCmd(cmd_hex) {}
	PTZInq(const char *cmd_hex, QList<datagram_field *> rslts) : PTZCmd(cmd_hex, {}, rslts) {}
};

extern int scale_speed(double speed, int max);
