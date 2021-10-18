/* Pan Tilt Zoom camera controls
 *
 * Copyright 2020 Grant Likely <grant.likely@secretlab.ca>
 *
 * SPDX-License-Identifier: GPLv2
 */

#include <obs-module.h>
#include <obs.hpp>
#include <util/config-file.h>
#include <util/platform.h>
#include <QMainWindow>
#include <QMenuBar>
#include <QVBoxLayout>
#include <QToolTip>
#include <QWindow>
#include <QResizeEvent>

#include "imported/qt-wrappers.hpp"
#include "ui_ptz-controls.h"
#include "ptz-controls.hpp"
#include "settings.hpp"
#include "ptz-visca.hpp"
#include "ptz-pelco.hpp"
#include "ptz.h"

void ptz_load_controls(void)
{
	const auto main_window =
		static_cast<QMainWindow *>(obs_frontend_get_main_window());
	obs_frontend_push_ui_translation(obs_module_get_string);
	auto *tmp = new PTZControls(main_window);
	obs_frontend_add_dock(tmp);
	obs_frontend_pop_ui_translation();
}

PTZControls* PTZControls::instance = NULL;

/**
 * class buttonResizeFilter - Event filter to adjust button minimum height and resize icon
 *
 * This filter will update the minimumHeight property to keep a button square
 * when possible. It will also resize the button icon to match the button size.
 */
class buttonResizeFilter:public QObject {
public:
	buttonResizeFilter(QObject *parent) : QObject(parent) {}
	bool eventFilter(QObject *watched, QEvent *event) override {
		if (event->type() != QEvent::Resize)
			return false;
		auto button = static_cast<QAbstractButton*>(watched);
		auto resEvent = static_cast<QResizeEvent*>(event);

		button->setMinimumHeight(resEvent->size().width());

		int size = resEvent->size().width() * 2/3;
		button->setIconSize(QSize(size, size));
		return true;
	}
};

void PTZControls::OBSFrontendEventWrapper(enum obs_frontend_event event, void *ptr)
{
	PTZControls *controls = reinterpret_cast<PTZControls *>(ptr);
	controls->OBSFrontendEvent(event);
}

void PTZControls::OBSFrontendEvent(enum obs_frontend_event event)
{
	obs_source_t *scene = NULL;
	const char *name;

	switch (event) {
	case OBS_FRONTEND_EVENT_SCENE_CHANGED:
		if (ui->targetButton_program->isChecked())
			scene = obs_frontend_get_current_scene();
		break;
	case OBS_FRONTEND_EVENT_PREVIEW_SCENE_CHANGED:
		if (ui->targetButton_preview->isChecked())
			scene = obs_frontend_get_current_preview_scene();
		break;
	default:
		break;
	}

	if (!scene)
		return;

	name = obs_source_get_name(scene);
	for (unsigned long int i = 0; i < ptzDeviceList.device_count(); i++) {
		PTZDevice *ptz = ptzDeviceList.get_device(i);
		if (ptz->objectName() == name) {
			setCurrent(i);
			break;
		}
	}

	obs_source_release(scene);
}

PTZControls::PTZControls(QWidget *parent)
	: QDockWidget(parent), ui(new Ui::PTZControls),
	  use_gamepad(false), gamepad(NULL)
{
	instance = this;
	ui->setupUi(this);
	ui->cameraList->setModel(&ptzDeviceList);

	QItemSelectionModel *selectionModel = ui->cameraList->selectionModel();
	connect(selectionModel,
		SIGNAL(currentChanged(QModelIndex, QModelIndex)),
		this, SLOT(currentChanged(QModelIndex, QModelIndex)));

	LoadConfig();

	ui->speedSlider->setValue(50);
	ui->speedSlider->setMinimum(0);
	ui->speedSlider->setMaximum(100);

	auto filter = new buttonResizeFilter(this);
	ui->panTiltButton_upleft->installEventFilter(filter);
	ui->panTiltButton_up->installEventFilter(filter);
	ui->panTiltButton_upright->installEventFilter(filter);
	ui->panTiltButton_left->installEventFilter(filter);
	ui->panTiltButton_home->installEventFilter(filter);
	ui->panTiltButton_right->installEventFilter(filter);
	ui->panTiltButton_downleft->installEventFilter(filter);
	ui->panTiltButton_down->installEventFilter(filter);
	ui->panTiltButton_downright->installEventFilter(filter);
	ui->zoomButton_wide->installEventFilter(filter);
	ui->zoomButton_tele->installEventFilter(filter);
	ui->focusButton_auto->installEventFilter(filter);
	ui->focusButton_near->installEventFilter(filter);
	ui->focusButton_far->installEventFilter(filter);
	ui->focusButton_onetouch->installEventFilter(filter);

	obs_frontend_add_event_callback(OBSFrontendEventWrapper, this);

	hide();

	/* loadHotkey helpers lifted from obs-studio/UI/window-basic-main.cpp */
	auto loadHotkeyData = [&](const char *name) -> OBSData {
		config_t *cfg = obs_frontend_get_profile_config();
		const char *info = config_get_string(cfg, "Hotkeys", name);
		if (!info)
			return {};
		obs_data_t *data = obs_data_create_from_json(info);
		if (!data)
			return {};
		OBSData res = data;
		obs_data_release(data);
		return res;
	};
	auto registerHotkey = [&](const char *name, const char *description,
			      obs_hotkey_func func, void *data) -> obs_hotkey_id {
		obs_hotkey_id id;

		id = obs_hotkey_register_frontend(name, description, func, data);
		obs_data_array_t *array =
			obs_data_get_array(loadHotkeyData(name), "bindings");
		obs_hotkey_load(id, array);
		obs_data_array_release(array);
		return id;
	};
	auto cb = [](void *data, obs_hotkey_id, obs_hotkey_t *, bool pressed) {
		QPushButton *button = static_cast<QPushButton*>(data);
		if (pressed)
			button->pressed();
		else
			button->released();
	};
	auto autofocustogglecb = [](void *data, obs_hotkey_id, obs_hotkey_t *, bool pressed) {
		PTZControls *ptzctrl = static_cast<PTZControls*>(data);
		if (pressed)
			ptzctrl->on_focusButton_auto_clicked(!ptzctrl->ui->focusButton_auto->isChecked());
	};
	hotkeys << registerHotkey("PTZ.PanTiltUpLeft", "PTZ Pan camera up & left",
				cb, ui->panTiltButton_upleft);
	hotkeys << registerHotkey("PTZ.PanTiltLeft", "PTZ Pan camera left",
				cb, ui->panTiltButton_left);
	hotkeys << registerHotkey("PTZ.PanTiltDownLeft", "PTZ Pan camera down & left",
				cb, ui->panTiltButton_downleft);
	hotkeys << registerHotkey("PTZ.PanTiltUpRight", "PTZ Pan camera up & right",
				cb, ui->panTiltButton_upright);
	hotkeys << registerHotkey("PTZ.PanTiltRight", "PTZ Pan camera right",
				cb, ui->panTiltButton_right);
	hotkeys << registerHotkey("PTZ.PanTiltDownRight", "PTZ Pan camera down & right",
				cb, ui->panTiltButton_downright);
	hotkeys << registerHotkey("PTZ.PanTiltUp", "PTZ Tilt camera up",
				cb, ui->panTiltButton_up);
	hotkeys << registerHotkey("PTZ.PanTiltDown", "PTZ Tilt camera down",
				cb, ui->panTiltButton_down);
	hotkeys << registerHotkey("PTZ.ZoomWide", "PTZ Zoom camera out (wide)",
				cb, ui->zoomButton_wide);
	hotkeys << registerHotkey("PTZ.ZoomTele", "PTZ Zoom camera in (telefocal)",
				cb, ui->zoomButton_tele);
	hotkeys << registerHotkey("PTZ.FocusAutoFocus", "PTZ Toggle Autofocus",
				autofocustogglecb, this);
	hotkeys << registerHotkey("PTZ.FocusNear", "PTZ Focus far",
				cb, ui->focusButton_far);
	hotkeys << registerHotkey("PTZ.FocusFar", "PTZ Focus near",
				cb, ui->focusButton_near);
	hotkeys << registerHotkey("PTZ.FocusOneTouch", "PTZ One touch focus trigger",
				cb, ui->focusButton_onetouch);

	auto preset_recall_cb = [](void *data, obs_hotkey_id hotkey, obs_hotkey_t *, bool pressed) {
		PTZControls *ptzctrl = static_cast<PTZControls*>(data);
		blog(LOG_INFO, "Recalling %i", ptzctrl->preset_hotkey_map[hotkey]);
		if (pressed)
			ptzctrl->presetRecall(ptzctrl->preset_hotkey_map[hotkey]);
	};

	for (int i = 0; i < 16; i++) {
		auto name = QString("PTZ.Recall%1").arg(i+1);
		auto description = QString("PTZ Memory Recall #%1").arg(i+1);
		obs_hotkey_id hotkey = registerHotkey(QT_TO_UTF8(name), QT_TO_UTF8(description),
							preset_recall_cb, this);
		preset_hotkey_map[hotkey] = i;
		hotkeys << hotkey;
	}
}

PTZControls::~PTZControls()
{
	while (!hotkeys.isEmpty())
		obs_hotkey_unregister(hotkeys.takeFirst());

	//signal_handler_disconnect_global(obs_get_signal_handler(), OBSSignal, this);
	SaveConfig();
	ptzDeviceList.delete_all();
	deleteLater();
}

/*
 * Save/Load configuration methods
 */
void PTZControls::SaveConfig()
{
	char *file = obs_module_config_path("config.json");
	if (!file)
		return;

	OBSData data = obs_data_create();
	obs_data_release(data);

	obs_data_set_string(data, "splitter_state",
			ui->splitter->saveState().toBase64().constData());

	obs_data_set_bool(data, "use_gamepad", use_gamepad);
	obs_data_set_int(data, "debug_log_level", ptz_debug_level);
	const char *target_mode = "manual";
	if (ui->targetButton_preview->isChecked())
		target_mode = "preview";
	if (ui->targetButton_program->isChecked())
		target_mode = "program";
	obs_data_set_string(data, "target_mode", target_mode);

	OBSDataArray camera_array = ptz_devices_get_config();
	obs_data_array_release(camera_array);
	obs_data_set_array(data, "devices", camera_array);

	/* Save data structure to json */
	if (!obs_data_save_json_safe(data, file, "tmp", "bak")) {
		char *path = obs_module_config_path("");
		if (path) {
			os_mkdirs(path);
			bfree(path);
		}
		obs_data_save_json_safe(data, file, "tmp", "bak");
	}
	bfree(file);
}

void PTZControls::LoadConfig()
{
	char *file = obs_module_config_path("config.json");
	OBSDataArray array;
	std::string target_mode;

	if (!file)
		return;

	OBSData data = obs_data_create_from_json_file_safe(file, "bak");
	bfree(file);
	if (!data)
		return;
	obs_data_release(data);
	obs_data_set_default_int(data, "debug_log_level", LOG_INFO);
	obs_data_set_default_bool(data, "use_gamepad", false);
	obs_data_set_default_string(data, "target_mode", "preview");

	ptz_debug_level = obs_data_get_int(data, "debug_log_level");
	use_gamepad = obs_data_get_bool(data, "use_gamepad");
	target_mode = obs_data_get_string(data, "target_mode");
	ui->targetButton_preview->setChecked(target_mode == "preview");
	ui->targetButton_program->setChecked(target_mode == "program");
	ui->targetButton_manual->setChecked(target_mode != "preview" && target_mode != "program");

	setGamepadEnabled(use_gamepad);

	const char *splitterStateStr = obs_data_get_string(data, "splitter_state");
	if (splitterStateStr) {
		QByteArray splitterState = QByteArray::fromBase64(QByteArray(splitterStateStr));
		ui->splitter->restoreState(splitterState);
	}

	array = obs_data_get_array(data, "devices");
	obs_data_array_release(array);
	ptz_devices_set_config(array);
}


void PTZControls::setGamepadEnabled(bool enable)
{
	use_gamepad = enable;

	if (!use_gamepad && gamepad) {
		disconnect(gamepad);
		delete gamepad;
		gamepad = NULL;
	}

	if (use_gamepad && !gamepad) {
		QGamepadManager* gpm = QGamepadManager::instance();
		// Windows workaround from StackOverflow:
		// https://stackoverflow.com/questions/62668629/qgamepadmanager-connecteddevices-empty-but-windows-detects-gamepad
		QWindow* wnd = new QWindow();
		wnd->show();
		delete wnd;
		qApp->processEvents();

		QList<int> gamepads = gpm->connectedGamepads();

		blog(LOG_INFO, "gamepads found %d", gamepads.size());
		Q_FOREACH(int id, gpm->connectedGamepads()) {
			if (!gpm->gamepadName(id).isEmpty())
				ptz_debug("Gamepad %i: %s", id, qPrintable(gpm->gamepadName(id)));
		}

		if (!gamepads.isEmpty()) {
			gamepad = new QGamepad(*gamepads.begin(), this);

			connect(gamepad, &QGamepad::axisLeftXChanged, this,
					&PTZControls::on_panTiltGamepad);
			connect(gamepad, &QGamepad::axisLeftYChanged, this,
					&PTZControls::on_panTiltGamepad);
		}
	}
}

PTZDevice * PTZControls::currCamera()
{
	return ptzDeviceList.get_device(current_cam);
}

void PTZControls::setPanTilt(double pan, double tilt)
{
	double speed = ui->speedSlider->value();
	PTZDevice *ptz = currCamera();
	if (!ptz)
		return;

	if (QGuiApplication::keyboardModifiers().testFlag(Qt::ControlModifier)) {
		ptz->pantilt(pan, tilt);
	} else if (QGuiApplication::keyboardModifiers().testFlag(Qt::ShiftModifier)) {
		ptz->pantilt_rel(pan, - tilt);
	} else {
		ptz->pantilt(pan * speed / 100, tilt * speed / 100);
	}
}

void PTZControls::on_panTiltGamepad()
{
	if (!gamepad)
		return;
	setPanTilt(gamepad->axisLeftX(), - gamepad->axisLeftY());
}

/* The pan/tilt buttons are a large block of simple and mostly identical code.
 * Use C preprocessor macro to create all the duplicate functions */
#define button_pantilt_actions(direction, x, y) \
	void PTZControls::on_panTiltButton_##direction##_pressed() \
	{ setPanTilt(x, y); } \
	void PTZControls::on_panTiltButton_##direction##_released() \
	{ PTZDevice *ptz = currCamera(); if (ptz) ptz->pantilt_stop(); }

button_pantilt_actions(up, 0, 1);
button_pantilt_actions(upleft, -1, 1);
button_pantilt_actions(upright, 1, 1);
button_pantilt_actions(left, -1, 0);
button_pantilt_actions(right, 1, 0);
button_pantilt_actions(down, 0, -1);
button_pantilt_actions(downleft, -1, -1);
button_pantilt_actions(downright, 1, -1);

void PTZControls::on_panTiltButton_home_released()
{
	PTZDevice *ptz = currCamera();
	if (ptz)
		ptz->pantilt_home();
}

/* There are fewer buttons for zoom or focus; so don't bother with macros */
void PTZControls::on_zoomButton_tele_pressed()
{
	double speed = ui->speedSlider->value();
	PTZDevice *ptz = currCamera();
	if (!ptz)
		return;

	if (QGuiApplication::keyboardModifiers().testFlag(Qt::ControlModifier))
		ptz->zoom_tele(1);
	else if (QGuiApplication::keyboardModifiers().testFlag(Qt::ShiftModifier))
		ptz->zoom_tele(0);
	else
		ptz->zoom_tele(speed / 100);
}
void PTZControls::on_zoomButton_tele_released()
{
	PTZDevice *ptz = currCamera();
	if (ptz)
		ptz->zoom_stop();
}
void PTZControls::on_zoomButton_wide_pressed()
{
	double speed = ui->speedSlider->value();
	PTZDevice *ptz = currCamera();
	if (!ptz)
		return;

	if (QGuiApplication::keyboardModifiers().testFlag(Qt::ControlModifier))
		ptz->zoom_wide(1);
	else if (QGuiApplication::keyboardModifiers().testFlag(Qt::ShiftModifier))
		ptz->zoom_wide(0);
	else
		ptz->zoom_wide(speed / 100);
}
void PTZControls::on_zoomButton_wide_released()
{
	PTZDevice *ptz = currCamera();
	if (ptz)
		ptz->zoom_stop();
}

void PTZControls::on_focusButton_auto_clicked(bool checked)
{
	setAutofocusEnabled(checked);
	PTZDevice *ptz = currCamera();
	if (ptz)
		ptz->set_autofocus(checked);
}

void PTZControls::on_focusButton_near_pressed()
{
	double speed = ui->speedSlider->value();
	PTZDevice *ptz = currCamera();
	if (ptz)
		ptz->focus_near(speed / 100);
}

void PTZControls::on_focusButton_near_released()
{
	PTZDevice *ptz = currCamera();
	if (ptz)
		ptz->focus_stop();
}

void PTZControls::on_focusButton_far_pressed()
{
	double speed = ui->speedSlider->value();
	PTZDevice *ptz = currCamera();
	if (ptz)
		ptz->focus_far(speed / 100);
}

void PTZControls::on_focusButton_far_released()
{
	PTZDevice *ptz = currCamera();
	if (ptz)
		ptz->focus_stop();
}

void PTZControls::on_focusButton_onetouch_clicked()
{
	PTZDevice *ptz = currCamera();
	if (ptz)
		ptz->focus_onetouch();
}

void PTZControls::full_stop()
{
	PTZDevice *ptz = currCamera();
	if (ptz) {
		ptz->pantilt_stop();
		ptz->zoom_stop();
	}
}

void PTZControls::setCurrent(unsigned int index)
{
	if (index == current_cam)
		return;
	full_stop();
	current_cam = index;
	ui->cameraList->setCurrentIndex(ptzDeviceList.index(current_cam, 0));
}

void PTZControls::on_targetButton_preview_clicked(bool checked)
{
	if (checked)
		OBSFrontendEvent(OBS_FRONTEND_EVENT_PREVIEW_SCENE_CHANGED);
}

void PTZControls::on_targetButton_program_clicked(bool checked)
{
	if (checked)
		OBSFrontendEvent(OBS_FRONTEND_EVENT_SCENE_CHANGED);
}

void PTZControls::setAutofocusEnabled(bool autofocus_on)
{
	ui->focusButton_auto->setChecked(autofocus_on);
	ui->focusButton_near->setFlat(autofocus_on);
	ui->focusButton_near->setEnabled(!autofocus_on);
	ui->focusButton_far->setFlat(autofocus_on);
	ui->focusButton_far->setEnabled(!autofocus_on);
	ui->focusButton_onetouch->setFlat(autofocus_on);
	ui->focusButton_onetouch->setEnabled(!autofocus_on);
}

void PTZControls::currentChanged(QModelIndex current, QModelIndex previous)
{
	Q_UNUSED(previous);
	full_stop();

	PTZDevice *ptz = ptzDeviceList.get_device(current_cam);
	if (ptz)
		disconnect(ptz, nullptr, this, nullptr);

	current_cam = current.row();
	ptz = ptzDeviceList.get_device(current_cam);
	ui->presetListView->setModel(ptz->presetModel());
	ptz->connect(ptz, SIGNAL(settingsChanged()), this, SLOT(settingsChanged()));

	auto settings = ptz->get_settings();
	setAutofocusEnabled(obs_data_get_bool(settings, "focus_af_enabled"));
}

void PTZControls::settingsChanged(OBSData settings)
{
	if (obs_data_has_user_value(settings, "focus_af_enabled"))
		setAutofocusEnabled(obs_data_get_bool(settings, "focus_af_enabled"));
}

void PTZControls::presetRecall(int id)
{
	PTZDevice *ptz = ptzDeviceList.get_device(current_cam);
	if (!ptz)
		return;
	ptz->memory_recall(id);
}

void PTZControls::on_presetListView_activated(QModelIndex index)
{
	presetRecall(index.row());
}

void PTZControls::on_presetListView_customContextMenuRequested(const QPoint &pos)
{
	QPoint globalpos = ui->presetListView->mapToGlobal(pos);
	QModelIndex index = ui->presetListView->indexAt(pos);
	PTZDevice *ptz = ptzDeviceList.get_device(current_cam);

	QMenu presetContext;
	QAction *renameAction = presetContext.addAction("Rename");
	QAction *setAction = presetContext.addAction("Save Preset");
	QAction *resetAction = presetContext.addAction("Clear Preset");
	QAction *action = presetContext.exec(globalpos);

	if (action == renameAction) {
		ui->presetListView->edit(index);
	} else if (action == setAction) {
		ptz->memory_set(index.row());
	} else if (action == resetAction) {
		ptz->memory_reset(index.row());
		ui->presetListView->model()->setData(index, QString("Preset %1").arg(index.row() + 1));
	}
}

void PTZControls::on_cameraList_doubleClicked(const QModelIndex &index)
{
	ptz_settings_show(index.row());
}

void PTZControls::on_cameraList_customContextMenuRequested(const QPoint &pos)
{
	QPoint globalpos = ui->cameraList->mapToGlobal(pos);
	QModelIndex index = ui->cameraList->indexAt(pos);
	PTZDevice *ptz = ptzDeviceList.get_device(index.row());

	OBSData settings = ptz->get_settings();

	QMenu context;
	bool power_on = obs_data_get_bool(settings, "power_on");
	QAction *powerAction = context.addAction(power_on ? "Power Off" : "Power On");

	QAction *wbOnetouchAction = nullptr;
	bool wb_onepush = (obs_data_get_int(settings, "wb_mode") == 3);
	if (wb_onepush)
		wbOnetouchAction = context.addAction("Trigger One-Push White Balance");
	QAction *action = context.exec(globalpos);

	OBSData data = obs_data_create();
	obs_data_release(data);

	if (action == powerAction) {
		obs_data_set_bool(data, "power_on", !power_on);
		ptz->set_settings(data);
	} else if (wb_onepush && action == wbOnetouchAction) {
		obs_data_set_bool(data, "wb_onepush_trigger", true);
		ptz->set_settings(data);
	}
}

void PTZControls::on_configButton_released()
{
	ptz_settings_show(ui->cameraList->currentIndex().row());
}
