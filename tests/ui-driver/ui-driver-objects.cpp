/* obs-ui-driver: naming Qt objects, and reading them as JSON
 *
 * Copyright 2026 Grant Likely <grant.likely@secretlab.ca>
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "ui-driver-objects.hpp"
#include "ui-driver.hpp"

#include <QApplication>
#include <QComboBox>
#include <QHash>
#include <QJsonArray>
#include <QJsonDocument>
#include <QPointer>
#include <QSet>
#include <QTabWidget>
#include <QWidget>

namespace {

/* Every object given an id, both ways round. An object leaves both when it
 * is destroyed; its id is never given to another. */
QHash<QObject *, QString> idOf;
QHash<QString, QPointer<QObject>> objectOf;
quint64 lastId = 0;

/* What a segment is about, for an error's message */
QString segmentText(const QJsonValue &segment)
{
	if (segment.isString())
		return segment.toString();
	return QString::fromUtf8(QJsonDocument(segment.toObject()).toJson(QJsonDocument::Compact));
}

/* Whether `object` is `ancestor`, or one of its descendants */
bool within(QObject *object, QObject *ancestor)
{
	for (QObject *o = object; o; o = o->parent())
		if (o == ancestor)
			return true;
	return false;
}

/* Whether `object` meets every criterion of `segment`: a string is its
 * objectName; an object has "class", "name", "id", and any other key a
 * property it has, with that value */
bool meets(QObject *object, const QJsonValue &segment)
{
	if (segment.isString())
		return object->objectName() == segment.toString();
	const QJsonObject criteria = segment.toObject();
	for (auto criterion = criteria.begin(); criterion != criteria.end(); ++criterion) {
		const QString key = criterion.key();
		if (key == "class") {
			if (!object->inherits(criterion.value().toString().toUtf8().constData()))
				return false;
		} else if (key == "name") {
			if (object->objectName() != criterion.value().toString())
				return false;
		} else if (key == "id") {
			if (objectOf.value(criterion.value().toString()) != object)
				return false;
		} else {
			QVariant value = object->property(key.toUtf8().constData());
			if (!value.isValid() || !matches(criterion.value(), value))
				return false;
		}
	}
	return true;
}

/* What a segment searches when it is the first: every window, and all that
 * is in it, each once although a dialog is in the window it belongs to too */
QList<QObject *> everything()
{
	QList<QObject *> all;
	QSet<QObject *> seen;
	auto add = [&](QObject *object) {
		if (!seen.contains(object)) {
			seen.insert(object);
			all.append(object);
		}
	};
	for (QWidget *window : QApplication::topLevelWidgets()) {
		add(window);
		for (QObject *child : window->findChildren<QObject *>())
			add(child);
	}
	return all;
}

bool isNumber(const QVariant &value)
{
	switch (value.typeId()) {
	case QMetaType::Int:
	case QMetaType::UInt:
	case QMetaType::LongLong:
	case QMetaType::ULongLong:
	case QMetaType::Double:
	case QMetaType::Float:
	case QMetaType::Short:
	case QMetaType::UShort:
	case QMetaType::Long:
	case QMetaType::ULong:
	case QMetaType::Char:
	case QMetaType::SChar:
	case QMetaType::UChar:
		return true;
	default:
		return value.metaType().flags().testFlag(QMetaType::IsEnumeration);
	}
}

} // namespace

QString objectId(QObject *object)
{
	auto known = idOf.constFind(object);
	if (known != idOf.constEnd())
		return *known;
	QString id = QStringLiteral("o%1").arg(++lastId);
	idOf.insert(object, id);
	objectOf.insert(id, object);
	QObject::connect(object, &QObject::destroyed, qApp, [object, id]() {
		idOf.remove(object);
		objectOf.remove(id);
	});
	return id;
}

Found resolve(const QJsonValue &path, bool all)
{
	Found found;
	if (!path.isArray() || path.toArray().isEmpty()) {
		found.error = error("bad_request", "\"path\" is not a list of segments");
		return found;
	}
	const QJsonArray segments = path.toArray();
	QObject *scope = nullptr;
	for (qsizetype i = 0; i < segments.size(); i++) {
		const QJsonValue segment = segments[i];
		if (!segment.isString() && !segment.isObject()) {
			found.error = error("bad_request", QString("segment %1 is neither a name nor criteria").arg(i));
			return found;
		}
		/* An id names one object straight away, which has only to be in scope */
		QList<QObject *> candidates;
		QString id = segment.isObject() ? segment.toObject().value("id").toString() : QString();
		if (!id.isEmpty()) {
			QObject *object = objectOf.value(id);
			if (object && (!scope || (object != scope && within(object, scope))))
				candidates.append(object);
		} else {
			candidates = scope ? scope->findChildren<QObject *>() : everything();
		}
		QList<QObject *> matching;
		for (QObject *object : candidates)
			if (meets(object, segment))
				matching.append(object);

		const bool last = i == segments.size() - 1;
		if (matching.isEmpty()) {
			found.error = error("not_found", QString("nothing is %1").arg(segmentText(segment)));
			return found;
		}
		if (matching.size() > 1 && !(last && all)) {
			found.error = error("ambiguous",
					    QString("%1 things are %2").arg(matching.size()).arg(segmentText(segment)));
			return found;
		}
		if (last)
			found.objects = matching;
		else
			scope = matching.first();
	}
	return found;
}

QJsonObject describe(QObject *object)
{
	QJsonObject described{{"id", objectId(object)},
			      {"class", object->metaObject()->className()},
			      {"name", object->objectName()}};
	if (auto widget = qobject_cast<QWidget *>(object)) {
		described["hidden"] = widget->isHidden();
		described["visible"] = widget->isVisible();
		described["enabled"] = widget->isEnabled();
	}
	if (object->metaObject()->indexOfProperty("text") >= 0)
		described["text"] = toJson(object->property("text"));
	return described;
}

bool matches(const QJsonValue &json, const QVariant &value)
{
	if (json.isDouble() && isNumber(value))
		return json.toDouble() == value.toDouble();
	if (json.isBool() && value.typeId() == QMetaType::Bool)
		return json.toBool() == value.toBool();
	return json.toVariant().toString() == value.toString();
}

QJsonValue toJson(const QVariant &value)
{
	if (!value.isValid())
		return QJsonValue::Null;
	QJsonValue json = QJsonValue::fromVariant(value);
	if (!json.isNull())
		return json;
	QString text = value.toString();
	return text.isEmpty() ? QJsonValue(QJsonValue::Null) : QJsonValue(text);
}

QJsonValue property(QObject *object, const QString &name, bool *found)
{
	*found = true;
	auto widget = qobject_cast<QWidget *>(object);
	if (name == "hidden" && widget)
		return widget->isHidden();
	if (name == "tabs") {
		if (auto tabs = qobject_cast<QTabWidget *>(object)) {
			QJsonArray list;
			for (int i = 0; i < tabs->count(); i++)
				list.append(QJsonObject{{"text", tabs->tabText(i)},
							{"visible", tabs->isTabVisible(i)},
							{"page",
							 tabs->widget(i) ? tabs->widget(i)->objectName() : QString()}});
			return list;
		}
	}
	if (name == "items") {
		if (auto combo = qobject_cast<QComboBox *>(object)) {
			QJsonArray list;
			for (int i = 0; i < combo->count(); i++)
				list.append(QJsonObject{{"text", combo->itemText(i)},
							{"data", toJson(combo->itemData(i))}});
			return list;
		}
	}
	QByteArray key = name.toUtf8();
	if (object->metaObject()->indexOfProperty(key.constData()) >= 0 || object->dynamicPropertyNames().contains(key))
		return toJson(object->property(key.constData()));
	*found = false;
	return QJsonValue::Null;
}
