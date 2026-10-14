/* obs-ui-driver: naming Qt objects, and reading them as JSON
 *
 * Copyright 2026 Grant Likely <grant.likely@secretlab.ca>
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * All of it is for the main thread only.
 */
#pragma once

#include <QJsonObject>
#include <QJsonValue>
#include <QList>
#include <QObject>
#include <QVariant>

/* What a path named: the objects, or why there are none (an error answer) */
struct Found {
	QList<QObject *> objects;
	QJsonObject error;
};

/* The objects `path` names (see README.md). Each segment but the last has to
 * name exactly one, and so does the last unless `all`. */
Found resolve(const QJsonValue &path, bool all = false);

/* The id the driver knows `object` by, the same for as long as it lives, and
 * never another's */
QString objectId(QObject *object);

/* {"id", "class", "name"}, with "hidden", "visible" and "enabled" for a
 * widget, and "text" for an object that has a text property */
QJsonObject describe(QObject *object);

/* Whether a value from a request is `value`: as numbers if both are, as
 * bools if both are, and otherwise as text */
bool matches(const QJsonValue &json, const QVariant &value);

/* `value` as JSON: what QJsonValue::fromVariant() makes of it, or its text if
 * that is nothing */
QJsonValue toJson(const QVariant &value);

/* The property `name` of `object`, as JSON, and whether it has one. Besides
 * its Qt properties, a QWidget has "hidden", a QTabWidget "tabs" and a
 * QComboBox "items" (see README.md). */
QJsonValue property(QObject *object, const QString &name, bool *found);
