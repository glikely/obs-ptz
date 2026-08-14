/* PTZ UI test harness: the preset API, called as another plugin would
 *
 * Copyright 2026 Grant Likely <grant.likely@secretlab.ca>
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include "ui-test-harness.hpp"

#include <obs.hpp>
#include <obs-module.h>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QList>
#include <QStringList>

#include "ptz-list-model.hpp"

namespace {

/* The device's source, found by the UUID the model has for it */
OBSSourceAutoRelease deviceSource(const QString &device)
{
	QModelIndex index = ptzUITestDeviceIndex(device);
	if (!index.isValid())
		return nullptr;
	QString uuid = index.data(PTZListModel::DeviceUuidRole).toString();
	return obs_get_source_by_uuid(qUtf8Printable(uuid));
}

bool writeJson(const QString &filename, const QJsonObject &object)
{
	OBSDataAutoRelease result = obs_data_create_from_json(QJsonDocument(object).toJson(QJsonDocument::Compact));
	return obs_data_save_json_safe(result, qUtf8Printable(filename), "tmp", "bak");
}

/* An obs_data_t as the JSON object it is */
QJsonObject dataToJson(obs_data_t *data)
{
	if (!data)
		return QJsonObject();
	return QJsonDocument::fromJson(obs_data_get_json(data)).object();
}

/* Calls a proc on the device's proc_handler with the calldata fields in args, a
 * JSON object: a string, number or bool goes in as that, and an object or array
 * as the obs_data_t it is, for a ptr field. What the proc returned is as
 * "return", of the type "returns" says: "string", "int", "bool", or "data"
 * for an obs_data_t, which it releases. */
void runCallProcTest(const QMap<QString, QString> &params)
{
	QString filename = params.value(QStringLiteral("filename"));
	if (filename.isEmpty()) {
		blog(LOG_INFO, "[ptz-ui-test] call_proc: missing filename");
		return;
	}

	QJsonObject result;
	OBSSourceAutoRelease source = deviceSource(params.value(QStringLiteral("device")));
	proc_handler_t *ph = source ? obs_source_get_proc_handler(source) : nullptr;
	QString proc = params.value(QStringLiteral("proc"));
	calldata_t cd = {};
	QList<obs_data_t *> held;
	QJsonObject args = QJsonDocument::fromJson(params.value(QStringLiteral("args"), "{}").toUtf8()).object();
	/* A JSON number can't say whether it is a float: 1.0 is the same as 1 */
	QStringList floats = params.value(QStringLiteral("floats")).split(',', Qt::SkipEmptyParts);
	for (auto it = args.begin(); it != args.end(); ++it) {
		QByteArray key = it.key().toUtf8();
		const QJsonValue &value = it.value();
		if (value.isString()) {
			calldata_set_string(&cd, key, qUtf8Printable(value.toString()));
		} else if (value.isBool()) {
			calldata_set_bool(&cd, key, value.toBool());
		} else if (value.isDouble() && !floats.contains(it.key()) &&
			   value.toDouble() == (double)(long long)value.toDouble()) {
			calldata_set_int(&cd, key, (long long)value.toDouble());
		} else if (value.isDouble()) {
			calldata_set_float(&cd, key, value.toDouble());
		} else if (value.isObject() || value.isArray()) {
			QJsonObject wrapped = value.isObject() ? value.toObject() : QJsonObject{{"_", value}};
			obs_data_t *data =
				obs_data_create_from_json(QJsonDocument(wrapped).toJson(QJsonDocument::Compact));
			held.append(data);
			calldata_set_ptr(&cd, key, data);
		}
	}

	/* ptz_preset_get_list gave an obs_data_array_t before version 0.2, which is not
	 * to be read as the obs_data_t it gives from then on */
	bool older = false;
	if (ph && proc == QStringLiteral("ptz_preset_get_list")) {
		calldata_t version = {};
		older = !proc_handler_call(ph, "ptz_get_api_version", &version) || calldata_int(&version, "minor") < 2;
		calldata_free(&version);
	}
	bool called = ph && !older && proc_handler_call(ph, qUtf8Printable(proc), &cd);
	result["called"] = called;
	QString returns = params.value(QStringLiteral("returns"));
	if (called && returns == QStringLiteral("string")) {
		const char *text = nullptr;
		calldata_get_string(&cd, "return", &text);
		result["return"] = QString::fromUtf8(text ? text : "");
	} else if (called && returns == QStringLiteral("int")) {
		result["return"] = (qint64)calldata_int(&cd, "return");
	} else if (called && returns == QStringLiteral("bool")) {
		result["return"] = calldata_bool(&cd, "return");
	} else if (called && returns == QStringLiteral("data")) {
		auto data = static_cast<obs_data_t *>(calldata_ptr(&cd, "return"));
		result["return"] = dataToJson(data);
		obs_data_release(data);
	}
	/* What a ptr the caller passed in came back as: the proc fills some in */
	QJsonObject passed;
	for (auto it = args.begin(); it != args.end(); ++it)
		if (it.value().isObject() || it.value().isArray())
			passed[it.key()] = dataToJson(static_cast<obs_data_t *>(calldata_ptr(&cd, it.key().toUtf8())));
	result["args"] = passed;

	calldata_free(&cd);
	for (obs_data_t *data : held)
		obs_data_release(data);
	if (!writeJson(filename, result))
		blog(LOG_INFO, "[ptz-ui-test] call_proc: failed to write %s", qUtf8Printable(filename));
}

/* What the device's preset signals have said since recording began, as JSON
 * objects of the signal's name and its fields */
QList<QJsonObject> recorded;

const char *const PRESET_SIGNALS[] = {"ptz_preset_added",   "ptz_preset_removed",    "ptz_preset_order_changed",
				      "ptz_preset_changed", "ptz_preset_list_reset", "ptz_preset_create_done"};

void recordSignal(void *data, calldata_t *cd)
{
	QJsonObject event;
	event["signal"] = QString::fromUtf8(static_cast<const char *>(data));
	const char *text = nullptr;
	if (calldata_get_string(cd, "id", &text) && text)
		event["id"] = QString::fromUtf8(text);
	if (calldata_get_string(cd, "request", &text) && text)
		event["request"] = QString::fromUtf8(text);
	for (const char *key : {"index", "from", "to"}) {
		long long value;
		if (calldata_get_int(cd, key, &value))
			event[key] = (qint64)value;
	}
	void *changed = nullptr;
	if (calldata_get_ptr(cd, "changed", &changed) && changed)
		event["changed"] = dataToJson(static_cast<obs_data_t *>(changed));
	recorded.append(event);
}

/* Starts recording a device's preset signals, from nothing unless it is asked to keep what
 * it has */
void runRecordPresetSignalsTest(const QMap<QString, QString> &params)
{
	if (params.value(QStringLiteral("keep")) != QStringLiteral("1"))
		recorded.clear();
	OBSSourceAutoRelease source = deviceSource(params.value(QStringLiteral("device")));
	if (!source)
		return;
	signal_handler_t *sh = obs_source_get_signal_handler(source);
	for (const char *name : PRESET_SIGNALS) {
		signal_handler_disconnect(sh, name, recordSignal, (void *)name);
		signal_handler_connect(sh, name, recordSignal, (void *)name);
	}
}

void runGetPresetSignalsTest(const QMap<QString, QString> &params)
{
	QString filename = params.value(QStringLiteral("filename"));
	if (filename.isEmpty())
		return;
	QJsonArray events;
	for (const QJsonObject &event : recorded)
		events.append(event);
	if (!writeJson(filename, QJsonObject{{"events", events}}))
		blog(LOG_INFO, "[ptz-ui-test] get_preset_signals: failed to write %s", qUtf8Printable(filename));
}

} // namespace

/* call_proc request params:
 *   device   - the device, by the UUID of its filter or the name of the source it is on
 *   proc     - the proc to call, "ptz_preset_create" say
 *   args     - a JSON object of the calldata fields to give it
 *   floats   - the args that are floats, comma-separated: a number with no fraction is an int
 *              unless it is here
 *   returns  - what it returns as "return": "string", "int", "bool" or "data" (an obs_data_t
 *              it releases); left out for nothing
 *   filename - where to write {"called", "return", "args"}, "called" being false if there was
 *              no such device or proc, and "args" the objects passed in, as the proc left them
 *
 * record_preset_signals request params:
 *   device   - the device, which it starts recording the ptz_preset_* signals of, from
 *              nothing
 *   keep     - "1" to keep what has been recorded, and only make sure this device is
 *              being recorded
 *
 * get_preset_signals request params:
 *   filename - where to write {"events": [{"signal", "id", "index", "from", "to",
 *              "request", "changed"}...]}, the fields each signal has, in the order they came
 */
void registerPresetApiTest(PTZUITestHarness *harness)
{
	harness->registerTest(QStringLiteral("call_proc"), &runCallProcTest);
	harness->registerTest(QStringLiteral("record_preset_signals"), &runRecordPresetSignalsTest);
	harness->registerTest(QStringLiteral("get_preset_signals"), &runGetPresetSignalsTest);
}
