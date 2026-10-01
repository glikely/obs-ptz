/* VISCA commands and inquiries
 *
 * Copyright 2020 Grant Likely <grant.likely@secretlab.ca>
 *
 * SPDX-License-Identifier: GPLv2
 */
#pragma once

#include <optional>
#include "protocol-helpers.hpp"

/* The ones the driver sends by name */
extern const PTZCmd VISCA_CAM_Power;
extern const PTZCmd VISCA_CAM_Zoom_drive;
extern const PTZCmd VISCA_CAM_Zoom_Direct;
extern const PTZCmd VISCA_CAM_Focus_drive;
extern const PTZCmd VISCA_CAM_Focus_OneTouch;
extern const PTZCmd VISCA_CAM_ZoomFocus_Direct;
extern const PTZCmd VISCA_CAM_AFMode_ActiveIntervalTime;
extern const PTZCmd VISCA_CAM_WB_OnePushTrigger;
extern const PTZCmd VISCA_CAM_Memory_Reset;
extern const PTZCmd VISCA_CAM_Memory_Set;
extern const PTZCmd VISCA_CAM_Memory_Recall;
extern const PTZCmd VISCA_CAM_Tally_On;
extern const PTZCmd VISCA_CAM_Tally_Off;
extern const PTZCmd VISCA_CAM_TallyGreen_On;
extern const PTZCmd VISCA_CAM_TallyGreen_Off;
extern const PTZCmd VISCA_PanTilt_drive;
extern const PTZCmd VISCA_PanTilt_drive_abs;
extern const PTZCmd VISCA_PanTilt_drive_rel;
extern const PTZCmd VISCA_PanTilt_Home;

/* A state value, and the commands and inquiries for it */
struct ViscaControl {
	/* The state key, e.g. "wb_mode" */
	QString key;
	/* The command that sets it, with the value as its one argument */
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
const char *visca_tally_key(const QByteArray &cmd);
