/* PTZ UI test harness: the PTZ settings dialog's settings and status views
 *
 * Copyright 2026 Grant Likely <grant.likely@secretlab.ca>
 *
 * SPDX-License-Identifier: GPLv2
 */
#include "ui-test-harness.hpp"

#include <obs.hpp>
#include <obs-module.h>
#include <QApplication>
#include <QCheckBox>
#include <QComboBox>
#include <QSpinBox>
#include <QPushButton>
#include <QWidget>

#include "ptz-list-model.hpp"
#include "ptz-controls.hpp"
#include "ptz-state-view.hpp"
#include "settings.hpp"

namespace {

int settingsRefreshes = 0;
int stateUpdatesAtOpen = 0;
PTZSettings *counted = nullptr;

PTZSettings *findDialog()
{
	for (QWidget *w : QApplication::topLevelWidgets())
		if (auto dialog = qobject_cast<PTZSettings *>(w))
			return dialog;
	return nullptr;
}

/* The dialog's settings view, a properties view, and its state view. Both
 * are members of the dialog, private to it, so they are found by type among
 * its descendants. */
OBSPropertiesView *settingsView(PTZSettings *dialog)
{
	return dialog->findChild<OBSPropertiesView *>();
}

PTZStateView *stateView(PTZSettings *dialog)
{
	return dialog->findChild<PTZStateView *>();
}

/* The state view's widget that shows a state key: each is named for its key */
QWidget *stateWidget(PTZSettings *dialog, const QString &key)
{
	PTZStateView *view = stateView(dialog);
	return view ? view->findChild<QWidget *>(key) : nullptr;
}

/* The list of white balance modes */
QComboBox *whiteBalanceList(PTZSettings *dialog)
{
	return qobject_cast<QComboBox *>(stateWidget(dialog, QStringLiteral("wb_mode")));
}

/* Opens the dialog on a device and starts counting, from zero, how often its
 * settings view redraws and how often the state view is changed */
void runOpenSettingsDialogTest(const QMap<QString, QString> &params)
{
	bool deviceIdOk = false;
	uint32_t deviceId = params.value(QStringLiteral("device_id")).toUInt(&deviceIdOk);
	QModelIndex index = deviceIdOk ? ptzDeviceList->indexFromDeviceId(deviceId) : QModelIndex();
	if (!index.isValid()) {
		blog(LOG_INFO, "[ptz-ui-test] open_settings_dialog: missing/invalid device_id");
		return;
	}

	ptz_settings_show(index);
	PTZSettings *dialog = findDialog();
	if (!dialog) {
		blog(LOG_INFO, "[ptz-ui-test] open_settings_dialog: no dialog");
		return;
	}
	if (counted != dialog) {
		counted = dialog;
		QObject::connect(settingsView(dialog), &OBSPropertiesView::PropertiesRefreshed, dialog,
				 []() { settingsRefreshes++; });
	}
	settingsRefreshes = 0;
	stateUpdatesAtOpen = stateView(dialog)->updateCount();
}

/* Reports what the two views hold: the keys the settings view edits and the
 * ones the state view shows, its "wb_mode"/"connected"/position values, how
 * often the settings view redrew and the state view changed since opening,
 * whether Apply is showing, and how many widgets the state view is made of,
 * with the identity of its white balance list, to tell whether an update
 * kept them or replaced them. */
void runGetSettingsDialogTest(const QMap<QString, QString> &params)
{
	QString filename = params.value(QStringLiteral("filename"));
	PTZSettings *dialog = findDialog();
	if (filename.isEmpty() || !dialog || !settingsView(dialog) || !stateView(dialog)) {
		blog(LOG_INFO, "[ptz-ui-test] get_settings_dialog: no filename or no dialog");
		return;
	}

	OBSDataArrayAutoRelease settingsKeys = obs_data_array_create();
	for (obs_data_item_t *item = obs_data_first(settingsView(dialog)->GetSettings()); item;
	     obs_data_item_next(&item)) {
		OBSDataAutoRelease entry = obs_data_create();
		obs_data_set_string(entry, "key", obs_data_item_get_name(item));
		obs_data_array_push_back(settingsKeys, entry);
	}

	PTZStateView *state = stateView(dialog);
	const QVariantMap shown = state->shownValues();
	OBSDataArrayAutoRelease stateKeys = obs_data_array_create();
	for (const QString &key : shown.keys()) {
		OBSDataAutoRelease entry = obs_data_create();
		obs_data_set_string(entry, "key", qUtf8Printable(key));
		obs_data_array_push_back(stateKeys, entry);
	}

	auto apply = dialog->findChild<QPushButton *>(QStringLiteral("applyButton"));

	OBSDataAutoRelease result = obs_data_create();
	obs_data_set_array(result, "settings_keys", settingsKeys);
	obs_data_set_array(result, "state_keys", stateKeys);
	OBSDataAutoRelease shownValues = obs_data_create();
	for (auto value = shown.cbegin(); value != shown.cend(); ++value) {
		QByteArray key = value.key().toUtf8();
		switch (value.value().typeId()) {
		case QMetaType::Bool:
			obs_data_set_bool(shownValues, key.constData(), value.value().toBool());
			break;
		case QMetaType::Int:
		case QMetaType::LongLong:
			obs_data_set_int(shownValues, key.constData(), value.value().toLongLong());
			break;
		case QMetaType::Double:
			obs_data_set_double(shownValues, key.constData(), value.value().toDouble());
			break;
		default:
			obs_data_set_string(shownValues, key.constData(), qUtf8Printable(value.value().toString()));
			break;
		}
	}
	obs_data_set_obj(result, "shown", shownValues);
	obs_data_set_int(result, "wb_mode", shown.value("wb_mode").toInt());
	obs_data_set_bool(result, "connected", shown.value("connected").toBool());
	for (const char *axis : {"pan", "tilt", "zoom", "focus"})
		obs_data_set_double(result, axis, shown.value(axis).toDouble());
	obs_data_set_int(result, "settings_refreshes", settingsRefreshes);
	obs_data_set_int(result, "state_updates", state->updateCount() - stateUpdatesAtOpen);
	obs_data_set_bool(result, "apply_visible", apply && apply->isVisibleTo(dialog));
	obs_data_set_int(result, "state_widgets", state->findChildren<QWidget *>().size());
	obs_data_set_int(result, "wb_widget", (long long)(quintptr)whiteBalanceList(dialog));
	auto diagnostics = state->findChild<QWidget *>(QStringLiteral("diagnostics"));
	obs_data_set_bool(result, "diagnostics_visible", diagnostics && diagnostics->isVisibleTo(dialog));
	if (!obs_data_save_json_safe(result, qUtf8Printable(filename), "tmp", "bak"))
		blog(LOG_INFO, "[ptz-ui-test] get_settings_dialog: failed to write %s", qUtf8Printable(filename));
}

/* Edits state view fields as a user would, each param a state key and the
 * value to give it: picks it in a list, which changes and then says the user
 * chose it (a list only does that for the user, not when it is set from the
 * code); clicks a checkbox that isn't already that way; or enters a number. */
void runEditDialogStateTest(const QMap<QString, QString> &params)
{
	PTZSettings *dialog = findDialog();
	for (auto param = params.cbegin(); dialog && param != params.cend(); ++param) {
		if (param.key() == QStringLiteral("cmd"))
			continue;
		QWidget *widget = stateWidget(dialog, param.key());
		QString value = param.value().toLower();
		bool on = value == QStringLiteral("true");
		if (auto combo = qobject_cast<QComboBox *>(widget)) {
			bool isNumber = false;
			int number = value.toInt(&isNumber, 0);
			int at = combo->findData(isNumber ? QVariant(number) : QVariant(on));
			if (at < 0)
				continue;
			combo->setCurrentIndex(at);
			emit combo->activated(at);
		} else if (auto box = qobject_cast<QCheckBox *>(widget)) {
			if (box->isChecked() != on)
				box->click();
		} else if (auto spin = qobject_cast<QSpinBox *>(widget)) {
			spin->setValue(value.toInt(nullptr, 0));
		} else {
			blog(LOG_INFO, "[ptz-ui-test] edit_dialog_state: no field for %s", qUtf8Printable(param.key()));
		}
	}
}

/* Presses one of the state view's buttons, by its object name
 * ("cameraReport", say), as a user would */
void runPressDialogButtonTest(const QMap<QString, QString> &params)
{
	PTZSettings *dialog = findDialog();
	PTZStateView *view = dialog ? stateView(dialog) : nullptr;
	auto button = view ? view->findChild<QPushButton *>(params.value(QStringLiteral("button"))) : nullptr;
	if (!button) {
		blog(LOG_INFO, "[ptz-ui-test] press_dialog_button: no such button");
		return;
	}
	button->click();
}

} // namespace

/* open_settings_dialog request params:
 *   device_id - the device to show
 * get_settings_dialog: filename - where to write the {"settings_keys",
 *   "state_keys": [{"key"}...], "wb_mode", "connected", "pan", "tilt", "zoom",
 *   "focus", "settings_refreshes", "state_updates", "apply_visible",
 *   "state_widgets", "wb_widget", "diagnostics_visible", "shown": {every
 *   shown state key: the value shown}} JSON result
 * edit_dialog_state: each param a state key ("wb_mode", say) and the value to
 *   give its field: "True"/"False", or a number
 * press_dialog_button: button - the object name of a state view button
 */
void registerSettingsDialogTest(PTZUITestHarness *harness)
{
	harness->registerTest(QStringLiteral("open_settings_dialog"), &runOpenSettingsDialogTest);
	harness->registerTest(QStringLiteral("get_settings_dialog"), &runGetSettingsDialogTest);
	harness->registerTest(QStringLiteral("edit_dialog_state"), &runEditDialogStateTest);
	harness->registerTest(QStringLiteral("press_dialog_button"), &runPressDialogButtonTest);
}
