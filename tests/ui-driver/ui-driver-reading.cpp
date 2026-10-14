/* obs-ui-driver: the requests that find objects and read them
 *
 * Copyright 2026 Grant Likely <grant.likely@secretlab.ca>
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "ui-driver-objects.hpp"
#include "ui-driver.hpp"

#include <QApplication>
#include <QJsonArray>
#include <QWidget>

namespace {

QJsonObject findRequest(const QJsonObject &request)
{
	Found found = resolve(request["path"], request["all"].toBool(false));
	if (!found.error.isEmpty())
		return found.error;
	QJsonArray objects;
	for (QObject *object : found.objects)
		objects.append(describe(object));
	return QJsonObject{{"objects", objects}};
}

QJsonObject getRequest(const QJsonObject &request)
{
	if (!request["properties"].isArray())
		return error("bad_request", "\"properties\" is not a list of names");
	Found found = resolve(request["path"]);
	if (!found.error.isEmpty())
		return found.error;
	QObject *object = found.objects.first();
	QJsonObject properties;
	const QJsonArray names = request["properties"].toArray();
	for (const QJsonValue name : names) {
		bool has = false;
		QJsonValue value = property(object, name.toString(), &has);
		if (!has)
			return error("no_property", QString("%1 has no property %2")
							    .arg(object->metaObject()->className(), name.toString()));
		properties[name.toString()] = value;
	}
	return QJsonObject{{"properties", properties}};
}

/* `object`, described, with its children to `depth` levels below it */
QJsonObject node(QObject *object, int depth)
{
	QJsonObject described = describe(object);
	if (depth > 0) {
		QJsonArray children;
		for (QObject *child : object->children())
			children.append(node(child, depth - 1));
		if (!children.isEmpty())
			described["children"] = children;
	}
	return described;
}

QJsonObject treeRequest(const QJsonObject &request)
{
	int depth = request["depth"].toInt(3);
	if (!request.contains("path")) {
		QJsonArray windows;
		for (QWidget *window : QApplication::topLevelWidgets())
			windows.append(node(window, depth));
		return QJsonObject{{"tree", windows}};
	}
	Found found = resolve(request["path"]);
	if (!found.error.isEmpty())
		return found.error;
	return QJsonObject{{"tree", node(found.objects.first(), depth)}};
}

} // namespace

void addReadingRequests()
{
	addRequest("find", findRequest);
	addRequest("get", getRequest);
	addRequest("tree", treeRequest);
}
