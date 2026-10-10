/* Pan Tilt Zoom Controls - finding the PTZ drivers by their OBS filter kinds
 *
 * Copyright 2026 Grant Likely <grant.likely@secretlab.ca>
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#pragma once

#include <QList>
#include <QString>
#include <obs.hpp>

/* A filter kind that makes PTZ devices */
struct PTZDriverKind {
	/* The id of the filter kind */
	QString kind;
	/* What OBS calls it: what to show a user choosing one */
	QString name;
	/* The kind's defaults, which are what a new device starts from */
	OBSData defaults;
};

/* The filter kinds OBS has that are PTZ drivers, in the order OBS lists them:
 * those whose defaults carry the version of the PTZ API (see
 * ptz_filter_defaults_mark()) with this plugin's major version and a minor
 * version at least as new. Nothing is constructed to find out. Call it when
 * the answer is needed, not before every module has loaded. */
QList<PTZDriverKind> ptz_driver_kinds();
