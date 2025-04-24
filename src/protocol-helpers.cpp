/* PTZ Protocol helper classes and functions
 *
 * Copyright 2020-2022 Grant Likely <grant.likely@secretlab.ca>
 *
 * SPDX-License-Identifier: GPLv2
 */

#include <QMap>
#include <QVariant>
#include <obs.hpp>
#include <qt-wrappers.hpp>
#include "protocol-helpers.hpp"

OBSData variantMapToOBSData(const QVariantMap &map)
{
	OBSDataAutoRelease data = obs_data_create();
	QMapIterator<QString, QVariant> iter(map);
	while (iter.hasNext()) {
		iter.next();
		switch (iter.value().typeId()) {
		case QMetaType::Int:
		case QMetaType::UInt:
		case QMetaType::Long:
		case QMetaType::LongLong:
		case QMetaType::Short:
		case QMetaType::ULong:
		case QMetaType::ULongLong:
		case QMetaType::UShort:
			obs_data_set_int(data, QT_TO_UTF8(iter.key()),
					 iter.value().toLongLong());
			break;
		case QMetaType::Float:
			obs_data_set_double(data, QT_TO_UTF8(iter.key()),
					    iter.value().toFloat());
			break;
		case QMetaType::QString:
			obs_data_set_string(
				data, QT_TO_UTF8(iter.key()),
				QT_TO_UTF8(iter.value().toString()));
			break;
		}
	}
	return data.Get();
}

QVariantMap OBSDataToVariantMap(const OBSData data)
{
	QVariantMap map;
	obs_data_item_t *item = NULL;

	for (item = obs_data_first(data); item; obs_data_item_next(&item)) {
		QString name = obs_data_item_get_name(item);
		if (!obs_data_item_has_user_value(item))
			continue;
		switch (obs_data_item_gettype(item)) {
		case OBS_DATA_STRING:
			map[name] = QString(obs_data_item_get_string(item));
			break;
		case OBS_DATA_NUMBER:
			if (obs_data_item_numtype(item) == OBS_DATA_NUM_INT)
				map[name] = obs_data_item_get_int(item);
			else
				map[name] = obs_data_item_get_double(item);
			break;
		case OBS_DATA_BOOLEAN:
			map[name] = obs_data_item_get_bool(item);
			break;
		default:
			/* OBS_DATA_NULL */
			/* OBS_DATA_OBJECT */
			/* OBS_DATA_ARRAY */
			break;
		}
	}
	return map;
}

void bool_field::encode(QByteArray &msg, int val)
{
	if (msg.size() < offset + 1)
		return;
	msg[offset] = (msg[offset] & ~mask) | (val ? mask : 0);
}

bool bool_field::decode(OBSData data, QByteArray &msg)
{
	if (msg.size() < offset + 1)
		return false;
	obs_data_set_bool(data, name, (msg[offset] & mask) != 0);
	return true;
}

int_field::int_field(const char *name, unsigned offset, unsigned int mask,
		     bool signextend)
	: datagram_field(name, offset), mask(mask)
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

void int_field::encode(QByteArray &msg, int val)
{
	unsigned int encoded = 0;
	unsigned int current_bit = 0;
	unsigned int wm;
	if (msg.size() < offset + size)
		return;
	for (wm = mask; wm; wm = wm >> 1, current_bit++) {
		if (wm & 1) {
			encoded |= (val & 1) << current_bit;
			val = val >> 1;
		}
	}
	wm = mask;
	for (int i = size - 1; i >= 0; i--) {
		msg[offset + i] = 0xff & ((~wm & msg[offset + i]) | encoded);
		wm >>= 8;
		encoded >>= 8;
	}
}

bool int_field::decode_int(int *val_, QByteArray &msg)
{
	unsigned int encoded = 0;
	int val = 0;
	unsigned int current_bit = 0;
	if (msg.size() < offset + size)
		return false;
	for (int i = 0; i < size; i++)
		encoded = encoded << 8 | msg[offset + i];
	for (unsigned int wm = mask; wm; wm >>= 1, encoded >>= 1) {
		if (wm & 1) {
			val |= (encoded & 1) << current_bit;
			current_bit++;
		}
	}
	*val_ = (val ^ extend_mask) - extend_mask;
	return true;
}

bool int_field::decode(OBSData data, QByteArray &msg)
{
	int val;
	bool rc = decode_int(&val, msg);
	if (rc)
		obs_data_set_int(data, name, val);
	return rc;
}

bool string_lookup_field::decode(OBSData data, QByteArray &msg)
{
	int val;
	bool rc = decode_int(&val, msg);
	if (!rc)
		return false;
	obs_data_set_string(data, name, lookup.value(val, "Unknown").c_str());
	return true;
}

void PTZCmd::encode(QList<int> arglist)
{
	for (int i = 0; i < arglist.size() && i < args.size(); i++)
		args[i]->encode(cmd, arglist[i]);
}

obs_data_t *PTZCmd::decode(QByteArray msg)
{
	obs_data_t *data = obs_data_create();
	for (auto field : results)
		field->decode(data, msg);
	return data;
}

/**
 * scale_speed() - Helper to translate normalized speed to VISCA int
 * speed: normalized speed in range [-1.0, 1.0]
 * max: Maximum integer value accepted by camera
 *
 * returns an integer value scaled to the range [-max, max]
 */
int scale_speed(double speed, int max)
{
	// Account for very low max value that might round below zero.
	// If speed is small, but non-zero, it should return 1 or -1
	// Multiplier is 128 to handle largest possible visca value
	if (abs(speed) < 1.0 / 256)
		return 0;
	return int(std::copysign(
		std::clamp(abs(speed) * max + 0.5, 1.0, double(max)), speed));
}
