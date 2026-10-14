/* obs-ui-driver: the requests that change objects, call them, and watch them
 *
 * Copyright 2026 Grant Likely <grant.likely@secretlab.ca>
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "ui-driver-objects.hpp"
#include "ui-driver.hpp"

#include <QAbstractItemView>
#include <QComboBox>
#include <QHash>
#include <QJsonArray>
#include <QJsonDocument>
#include <QMetaMethod>
#include <QPointer>

namespace {

/* Counts a signal of the object it is a child of, and goes when that does */
class Counter : public QObject {
	Q_OBJECT

public:
	using QObject::QObject;
	int count = 0;

public slots:
	void hit() { count++; }
};

/* The watches there have been, by id, and the id of each object's signal's */
QHash<QString, QPointer<Counter>> watches;
QHash<QString, QString> watchOf;
quint64 lastWatch = 0;

/* The one object a request's path names, or the answer saying why there isn't one */
QObject *one(const QJsonObject &request, QJsonObject *failed)
{
	Found found = resolve(request["path"]);
	if (!found.error.isEmpty()) {
		*failed = found.error;
		return nullptr;
	}
	return found.objects.first();
}

/* `json` as a value of `type`, or an invalid QVariant if it can't be one */
QVariant converted(const QJsonValue &json, QMetaType type)
{
	QVariant value = json.toVariant();
	if (!value.isValid())
		return QVariant();
	if (value.metaType() != type && !value.convert(type))
		return QVariant();
	return value;
}

QJsonObject setRequest(const QJsonObject &request)
{
	if (!request["properties"].isObject())
		return error("bad_request", "\"properties\" is not an object of names and values");
	QJsonObject failed;
	QObject *object = one(request, &failed);
	if (!object)
		return failed;
	const QJsonObject properties = request["properties"].toObject();
	for (auto entry = properties.begin(); entry != properties.end(); ++entry) {
		const QMetaObject *meta = object->metaObject();
		int index = meta->indexOfProperty(entry.key().toUtf8().constData());
		if (index < 0)
			return error("no_property",
				     QString("%1 has no property %2").arg(meta->className(), entry.key()));
		QMetaProperty property = meta->property(index);
		if (!property.isWritable())
			return error("read_only", QString("%1's %2 can't be set").arg(meta->className(), entry.key()));
		QVariant value = converted(entry.value(), property.metaType());
		if (!value.isValid() || !property.write(object, value))
			return error(
				"bad_value",
				QString("%1's %2 can't be %3")
					.arg(meta->className(), entry.key(),
					     QJsonDocument(QJsonArray{entry.value()}).toJson(QJsonDocument::Compact)));
	}
	return QJsonObject();
}

QJsonObject invokeRequest(const QJsonObject &request)
{
	const QString name = request["method"].toString();
	const QJsonArray args = request["args"].toArray();
	const bool queued = request["queued"].toBool(false);
	if (name.isEmpty())
		return error("bad_request", "\"method\" is not a method's name");
	if (args.size() > 4)
		return error("bad_request", "a method is called with no more than 4 arguments");
	QJsonObject failed;
	QObject *object = one(request, &failed);
	if (!object)
		return failed;

	/* The first slot, or invokable method, of that name that takes that many arguments */
	const QMetaObject *meta = object->metaObject();
	QMetaMethod method;
	for (int i = 0; i < meta->methodCount(); i++) {
		QMetaMethod candidate = meta->method(i);
		if (candidate.methodType() != QMetaMethod::Slot && candidate.methodType() != QMetaMethod::Method)
			continue;
		if (QString::fromUtf8(candidate.name()) == name && candidate.parameterCount() == args.size()) {
			method = candidate;
			break;
		}
	}
	if (!method.isValid())
		return error("no_method", QString("%1 has no method %2 that takes %3 arguments")
						  .arg(meta->className(), name)
						  .arg(args.size()));

	QVariant values[4];
	QGenericArgument generic[4];
	for (qsizetype i = 0; i < args.size(); i++) {
		values[i] = converted(args[i], method.parameterMetaType(int(i)));
		if (!values[i].isValid())
			return error("bad_value", QString("argument %1 of %2 is not a %3")
							  .arg(i)
							  .arg(name, method.parameterMetaType(int(i)).name()));
		generic[i] = QGenericArgument(values[i].metaType().name(), values[i].constData());
	}

	if (queued) {
		/* Answered now: a method that opens a modal dialog doesn't return until it closes */
		method.invoke(object, Qt::QueuedConnection, generic[0], generic[1], generic[2], generic[3]);
		return QJsonObject();
	}
	QMetaType returnType = method.returnMetaType();
	if (!returnType.isValid() || returnType.id() == QMetaType::Void) {
		method.invoke(object, Qt::DirectConnection, generic[0], generic[1], generic[2], generic[3]);
		return QJsonObject();
	}
	QVariant returned(returnType);
	method.invoke(object, Qt::DirectConnection, QGenericReturnArgument(returnType.name(), returned.data()),
		      generic[0], generic[1], generic[2], generic[3]);
	return QJsonObject{{"return", toJson(returned)}};
}

QJsonObject activateRequest(const QJsonObject &request)
{
	if (!request.contains("data") && !request.contains("text"))
		return error("bad_request", "an item is chosen by its \"data\" or its \"text\"");
	QJsonObject failed;
	QObject *object = one(request, &failed);
	if (!object)
		return failed;
	auto combo = qobject_cast<QComboBox *>(object);
	if (!combo)
		return error("bad_request", QString("a %1 is not a combo box").arg(object->metaObject()->className()));

	for (int i = 0; i < combo->count(); i++) {
		bool chosen = request.contains("data") ? matches(request["data"], combo->itemData(i))
						       : request["text"].toString() == combo->itemText(i);
		if (!chosen)
			continue;
		/* As a user's choice: the list says it was chosen, which it doesn't for a
		 * change made in code */
		combo->setCurrentIndex(i);
		emit combo->activated(i);
		emit combo->textActivated(combo->itemText(i));
		return QJsonObject{{"index", i}};
	}
	return error("not_found", "the combo box has no such item");
}

QJsonObject selectRequest(const QJsonObject &request)
{
	if (!request.contains("value"))
		return error("bad_request", "a row is selected by its \"value\"");
	QJsonObject failed;
	QObject *object = one(request, &failed);
	if (!object)
		return failed;
	auto view = qobject_cast<QAbstractItemView *>(object);
	if (!view || !view->model() || !view->selectionModel())
		return error("bad_request",
			     QString("a %1 is not a view of a model").arg(object->metaObject()->className()));

	QAbstractItemModel *model = view->model();
	const QByteArray roleName = request["role"].toString("display").toUtf8();
	const QHash<int, QByteArray> roles = model->roleNames();
	int role = roles.key(roleName, -1);
	if (role < 0)
		return error("bad_value", QString("the model has no role %1").arg(QString::fromUtf8(roleName)));
	int column = request["column"].toInt(0);

	for (int row = 0; row < model->rowCount(view->rootIndex()); row++) {
		QModelIndex index = model->index(row, column, view->rootIndex());
		if (!matches(request["value"], index.data(role)))
			continue;
		/* As a click does */
		view->selectionModel()->setCurrentIndex(index, QItemSelectionModel::ClearAndSelect |
								       QItemSelectionModel::Rows);
		return QJsonObject{{"row", row}};
	}
	return error("not_found", "the view has no such row");
}

QJsonObject watchRequest(const QJsonObject &request)
{
	const QString name = request["signal"].toString();
	QJsonObject failed;
	QObject *object = one(request, &failed);
	if (!object)
		return failed;
	const QMetaObject *meta = object->metaObject();
	QMetaMethod signal;
	for (int i = 0; i < meta->methodCount(); i++) {
		QMetaMethod candidate = meta->method(i);
		if (candidate.methodType() == QMetaMethod::Signal && QString::fromUtf8(candidate.name()) == name) {
			signal = candidate;
			break;
		}
	}
	if (!signal.isValid())
		return error("no_method", QString("%1 has no signal %2").arg(meta->className(), name));

	/* Watching it again counts again from nothing */
	const QString key = objectId(object) + "/" + QString::fromUtf8(signal.methodSignature());
	QString id = watchOf.value(key);
	if (!id.isEmpty() && watches.value(id)) {
		watches.value(id)->count = 0;
		return QJsonObject{{"watch", id}};
	}
	auto counter = new Counter(object);
	int hit = counter->metaObject()->indexOfSlot("hit()");
	if (!QObject::connect(object, signal, counter, counter->metaObject()->method(hit))) {
		delete counter;
		return error("no_method", QString("%1's signal %2 can't be watched").arg(meta->className(), name));
	}
	if (id.isEmpty()) {
		id = QStringLiteral("w%1").arg(++lastWatch);
		watchOf.insert(key, id);
	}
	watches.insert(id, counter);
	return QJsonObject{{"watch", id}};
}

QJsonObject countRequest(const QJsonObject &request)
{
	QPointer<Counter> counter = watches.value(request["watch"].toString());
	if (!counter)
		return error("not_found", "no such watch, or what it watched has gone");
	return QJsonObject{{"count", counter->count}};
}

} // namespace

void addActionRequests()
{
	addRequest("set", setRequest);
	addRequest("invoke", invokeRequest);
	addRequest("activate", activateRequest);
	addRequest("select", selectRequest);
	addRequest("watch", watchRequest);
	addRequest("count", countRequest);
}

#include "ui-driver-actions.moc"
