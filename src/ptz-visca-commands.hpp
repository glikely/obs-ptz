/* VISCA commands and inquiries
 *
 * Copyright 2020 Grant Likely <grant.likely@secretlab.ca>
 *
 * SPDX-License-Identifier: GPLv2
 */
#pragma once

#include <optional>
#include "protocol-helpers.hpp"

/* A state value, and the commands and inquiries for it */
struct ViscaControl {
	/* The state key, e.g. "wb_mode" */
	QString key;
	/* The command that sets it: its arguments are the values its fields
	 * are named for, this one and any others it sets along with it */
	std::optional<PTZCmd> set;
	/* Or, where each value has a command of its own, those */
	QMap<int, PTZCmd> setTo;
	/* What reads it, best first: the next is for a camera that answers
	 * one with a syntax error. A block inquiry reads many values. */
	QList<PTZInq> reads;

	ViscaControl(const QString &key, const std::optional<PTZCmd> &set, const QList<PTZInq> &reads = {})
		: key(key),
		  set(set),
		  reads(reads)
	{
	}
	ViscaControl(const QString &key, const QMap<int, PTZCmd> &setTo, const QList<PTZInq> &reads = {})
		: key(key),
		  setTo(setTo),
		  reads(reads)
	{
	}
	bool settable() const { return set || !setTo.isEmpty(); }
};

extern const QList<ViscaControl> visca_controls;
const ViscaControl *visca_control(const QString &key);
extern const QMap<QString, PTZCmd> visca_actions;
extern const PTZCmd VISCA_CAM_WB_OnePushTrigger;
