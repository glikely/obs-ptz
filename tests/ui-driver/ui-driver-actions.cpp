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

/* The answer refusing what would give away what is in a secret */
QJsonObject secret(QObject *object, const QString &what)
{
	return error("secret", QString("%1 is a secret: %2").arg(object->metaObject()->className(), what));
}

/* The role a request names: a name from the model's roleNames(), "accessibleText"
 * or "accessibleDescription" (which a model answers without naming them, as OBS's
 * sources list does), or the role's number. -1 if the model has no such role. */
int roleNamed(const QAbstractItemModel *model, const QJsonValue &name)
{
	if (name.isDouble())
		return name.toInt();
	const QByteArray text = name.toString().toUtf8();
	int role = model->roleNames().key(text, -1);
	if (role >= 0)
		return role;
	if (text == "accessibleText")
		return Qt::AccessibleTextRole;
	if (text == "accessibleDescription")
		return Qt::AccessibleDescriptionRole;
	return -1;
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
	/* A secret is written to, but not shown */
	if (isSecret(object) && properties.contains("echoMode"))
		return secret(object, "its echoMode is not set");
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
	/* Nothing a secret's method gives back is given, as it may be what is in
	 * it (QLineEdit's inputMethodQuery() is), and what is in it is not put
	 * where anything is pasted from. Nothing is pasted, as what is may be a
	 * secret, copied by a button that copies it. */
	if (name == "paste")
		return secret(object, "nothing is pasted, as it may be one");
	if (isSecret(object)) {
		if (name == "copy" || name == "cut")
			return secret(object, "nothing is copied from it");
		QMetaType returns = method.returnMetaType();
		if (!queued && returns.isValid() && returns.id() != QMetaType::Void)
			return secret(object, QString("what %1 gives back is not given").arg(name));
	}

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
	/* Which item is chosen says what the items are */
	if (isSecret(object))
		return secret(object, "its items are not chosen from");
	auto combo = qobject_cast<QComboBox *>(object);
	if (!combo)
		return error("bad_request", QString("a %1 is not a combo box").arg(object->metaObject()->className()));

	for (int i = 0; i < combo->count(); i++) {
		bool chosen = request.contains("data") ? matches(request["data"], combo->itemData(i))
						       : request["text"].toString() == combo->itemText(i);
		if (!chosen)
			continue;
		/* As a user's choice: the list says it was chosen, which it doesn't for a
		 * change made in code. What is told of it may delete the list. */
		const QString text = combo->itemText(i);
		QPointer<QComboBox> list(combo);
		list->setCurrentIndex(i);
		if (list)
			emit list->activated(i);
		if (list)
			emit list->textActivated(text);
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
	if (isSecret(object))
		return secret(object, "its rows are not chosen from");
	auto view = qobject_cast<QAbstractItemView *>(object);
	if (!view || !view->model() || !view->selectionModel())
		return error("bad_request",
			     QString("a %1 is not a view of a model").arg(object->metaObject()->className()));

	QAbstractItemModel *model = view->model();
	const QJsonValue roleName = request.contains("role") ? request["role"] : QJsonValue("display");
	int role = roleNamed(model, roleName);
	if (role < 0)
		return error("bad_value", QString("the model has no role %1").arg(roleName.toVariant().toString()));
	int column = request["column"].toInt(0);

	for (int row = 0; row < model->rowCount(view->rootIndex()); row++) {
		QModelIndex index = model->index(row, column, view->rootIndex());
		QVariant data = index.data(role);
		if (!data.isValid() || !matches(request["value"], data))
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

/* The rows of a view or a combo box, as it shows them: for each, the value of
 * every role asked for ("flags" is the row's item flags), and which row is
 * current and which are selected */
QJsonObject rowsRequest(const QJsonObject &request)
{
	QJsonObject failed;
	QObject *object = one(request, &failed);
	if (!object)
		return failed;
	if (isSecret(object))
		return secret(object, "its rows are not read");

	QAbstractItemModel *model = nullptr;
	QModelIndex root;
	QAbstractItemView *view = qobject_cast<QAbstractItemView *>(object);
	QComboBox *combo = qobject_cast<QComboBox *>(object);
	if (view) {
		model = view->model();
		root = view->rootIndex();
	} else if (combo) {
		model = combo->model();
		root = combo->rootModelIndex();
	} else {
		return error("bad_request",
			     QString("a %1 is not a view or a combo box").arg(object->metaObject()->className()));
	}
	/* One with no model, as a view is while it has nothing to show, has no rows */
	if (!model)
		return QJsonObject{{"rows", QJsonArray()}, {"current", -1}, {"selected", QJsonArray()}};

	constexpr int FlagsRole = -2;
	QList<QPair<QString, int>> roles;
	const QJsonArray asked = request["roles"].isArray() ? request["roles"].toArray() : QJsonArray{"display"};
	for (const QJsonValue name : asked) {
		int role = name.toString() == "flags" ? FlagsRole : roleNamed(model, name);
		if (role == -1)
			return error("bad_value", QString("the model has no role %1").arg(name.toVariant().toString()));
		roles.append({name.toVariant().toString(), role});
	}
	int column = request["column"].toInt(0);

	QJsonArray rows;
	for (int row = 0; row < model->rowCount(root); row++) {
		QModelIndex index = model->index(row, column, root);
		QJsonObject values;
		for (const auto &[name, role] : roles)
			values[name] = role == FlagsRole ? QJsonValue(int(model->flags(index)))
							 : toJson(index.data(role));
		rows.append(values);
	}
	QJsonArray selected;
	int currentRow = -1;
	if (view) {
		if (view->selectionModel())
			for (const QModelIndex &index : view->selectionModel()->selectedRows(column))
				if (index.parent() == root)
					selected.append(index.row());
		QModelIndex current = view->currentIndex();
		if (current.isValid() && current.parent() == root)
			currentRow = current.row();
	} else {
		currentRow = combo->currentIndex();
		if (currentRow >= 0)
			selected.append(currentRow);
	}
	return QJsonObject{{"rows", rows}, {"current", currentRow}, {"selected", selected}};
}

} // namespace

void addActionRequests()
{
	addRequest("set", setRequest);
	addRequest("invoke", invokeRequest);
	addRequest("activate", activateRequest);
	addRequest("select", selectRequest);
	addRequest("rows", rowsRequest);
	addRequest("watch", watchRequest);
	addRequest("count", countRequest);
}

#include "ui-driver-actions.moc"
