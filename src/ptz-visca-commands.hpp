/* VISCA commands and inquiries
 *
 * Copyright 2020 Grant Likely <grant.likely@secretlab.ca>
 *
 * SPDX-License-Identifier: GPLv2
 */
#pragma once

#include <functional>
#include <memory>
#include <optional>
#include <QDir>
#include <QJsonObject>
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

/* A camera's command set: its state values, what the driver moves it and
 * uses its presets with (actions), and the commands sent as they are by the
 * ptz_trigger proc (triggers) */
struct ViscaProfile {
	/* What the "visca_profile" setting has for it, and what is shown */
	QString id;
	QString name;
	/* The cameras it is for, by (vendor ID << 16) | model ID, and by
	 * vendor ID alone, for a vendor whose model IDs vary */
	QList<int> models;
	QList<int> vendors;
	/* How far the camera goes, by "pan", "tilt", "zoom" and "focus": the
	 * position at one end of each (left, down, wide and far) and the other.
	 * Ones it doesn't say are the driver's defaults. */
	QMap<QString, QPair<int, int>> ranges;
	QList<ViscaControl> controls;
	QMap<QString, PTZCmd> actions;
	QMap<QString, PTZCmd> triggers;
	/* What the camera can be asked for while it is in standby, if not
	 * everything: some won't wake after being asked for anything else */
	std::optional<QSet<QString>> standby_reads;

	const ViscaControl *control(const QString &key) const;
	ViscaControl *control(const QString &key);
	/* Takes away a control, action or trigger */
	void remove(const QString &key);
};

std::shared_ptr<const ViscaProfile> visca_generic_profile();
/* The command sets there are, and the one for a camera by its model: the
 * generic one if there is none for it */
QList<std::shared_ptr<const ViscaProfile>> visca_profiles();
std::shared_ptr<const ViscaProfile> visca_profile_for_model(int vendor_id, int model_id);
std::shared_ptr<const ViscaProfile> visca_profile(const QString &id);
/* The command sets in `dir`, read as the ones shipped with the plugin are:
 * each can extend the generic one, or another in `dir`. Why any that can't
 * be read can't be is in `errors`, by its file's name. */
QList<std::shared_ptr<const ViscaProfile>> visca_load_shipped_profiles(const QDir &dir, QMap<QString, QString> *errors);
/* A command set read from JSON. `base` finds the one it extends, by its id.
 * Null if it isn't one, and why in `error`. One `shipped` with the plugin
 * can have controls and triggers the one it extends doesn't; a user's, only
 * ones named "user_...". */
std::shared_ptr<const ViscaProfile>
visca_profile_from_json(const QJsonObject &json,
			const std::function<std::shared_ptr<const ViscaProfile>(const QString &)> &base, QString *error,
			bool shipped = false);
