/* Pan Tilt Zoom camera controls
 *
 * Copyright 2020 Grant Likely <grant.likely@secretlab.ca>
 *
 * SPDX-License-Identifier: GPLv2
 */
#include "ptz-controls.hpp"
#include <QToolTip>

#include <obs-module.h>
#include <util/platform.h>
#include <QMainWindow>
#include <QMenuBar>
#include <QVBoxLayout>
#include "ui_ptz-controls.h"

#if LIBVISCA_FOUND
#include "ptz-visca.hpp"
#endif

OBS_DECLARE_MODULE()
OBS_MODULE_AUTHOR("Grant Likely <grant.likely@secretlab.ca>");
OBS_MODULE_USE_DEFAULT_LOCALE("ptz-controls", "en-US")

bool obs_module_load()
{
	const auto main_window =
		static_cast<QMainWindow *>(obs_frontend_get_main_window());
	obs_frontend_push_ui_translation(obs_module_get_string);
	auto *tmp = new PTZControls(main_window);
	obs_frontend_add_dock(tmp);
	obs_frontend_pop_ui_translation();
	return true;
}

void obs_module_unload() {}

MODULE_EXPORT const char *obs_module_description(void)
{
	return obs_module_text("Pan, Tilt & Zoom control plugin");
}

MODULE_EXPORT const char *obs_module_name(void)
{
	return obs_module_text("PTZ Controls");
}

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
	for (unsigned long int i = 0; i < PTZDevice::device_count(); i++) {
		struct PTZDevice *ptz = PTZDevice::get_device(i);
		if (ptz->objectName() == name) {
			setCurrent(i);
			break;
		}
	}

	obs_source_release(scene);
}

PTZControls::PTZControls(QWidget *parent)
	: QDockWidget(parent),
	  ui(new Ui::PTZControls),
	  gamepad(0)
{
	ui->setupUi(this);
	ui->cameraList->setModel(PTZDevice::model());

	LoadConfig();

	auto gamepads = QGamepadManager::instance()->connectedGamepads();
	if (!gamepads.isEmpty()) {
		gamepad = new QGamepad(*gamepads.begin(), this);

		connect(gamepad, &QGamepad::axisLeftXChanged, this,
				&PTZControls::on_panTiltGamepad);
		connect(gamepad, &QGamepad::axisLeftYChanged, this,
				&PTZControls::on_panTiltGamepad);
	}

	connect(ui->dockWidgetContents, &QWidget::customContextMenuRequested,
		this, &PTZControls::ControlContextMenu);

	obs_frontend_add_event_callback(OBSFrontendEventWrapper, this);

	hide();
}

PTZControls::~PTZControls()
{
	//signal_handler_disconnect_global(obs_get_signal_handler(), OBSSignal, this);
	SaveConfig();
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
	obs_data_t *data = obs_data_create_from_json_file(file);

	if (!data)
		data = obs_data_create();
	if (!data) {
		bfree(file);
		return;
	}

	obs_data_set_bool(data, "use_joystick", true);
	const char *target_mode = "manual";
	if (ui->targetButton_preview->isChecked())
		target_mode = "preview";
	if (ui->targetButton_program->isChecked())
		target_mode = "program";
	obs_data_set_string(data, "target_mode", target_mode);
	obs_data_erase(data, "cameras");
	obs_data_array_t *camera_array = obs_data_array_create();
	obs_data_set_array(data, "cameras", camera_array);
	for (unsigned long int i = 0; i < PTZDevice::device_count(); i++) {
		PTZDevice *ptz = PTZDevice::get_device(i);
		if (!ptz)
			continue;
		obs_data_t *ptz_data = obs_data_create();
		ptz->get_config(ptz_data);
		obs_data_array_push_back(camera_array, ptz_data);
	}

	/* Save data structure to json */
	if (!obs_data_save_json(data, file)) {
		char *path = obs_module_config_path("");
		if (path) {
			os_mkdirs(path);
			bfree(path);
		}
		obs_data_save_json(data, file);
	}
	obs_data_release(data);
	bfree(file);
}

void PTZControls::LoadConfig()
{
	char *file = obs_module_config_path("config.json");
	obs_data_t *data = nullptr;
	obs_data_array_t *array;
	std::string target_mode;

	if (file) {
		data = obs_data_create_from_json_file(file);
		bfree(file);
	}
	if (!data) {
		qDebug() << "No configuration data; creating test devices";
		OpenInterface();
		return;
	}

	target_mode = obs_data_get_string(data, "target_mode");
	if (target_mode == "preview")
		ui->targetButton_preview->setChecked(true);
	if (target_mode == "program")
		ui->targetButton_program->setChecked(true);

	array = obs_data_get_array(data, "cameras");
	if (!array) {
		qDebug() << "No camera array; creating test devices";
		OpenInterface();
		return;
	}
	for (size_t i = 0; i < obs_data_array_count(array); i++) {
		obs_data_t *ptzcfg = obs_data_array_item(array, i);
		if (!ptzcfg) {
			qDebug() << "Config problem!";
			continue;
		}

		qDebug() << "creating camera" << obs_data_get_string(ptzcfg, "type") << obs_data_get_string(ptzcfg, "name");
		PTZDevice *ptz = PTZDevice::make_device(ptzcfg);
	}
}

void PTZControls::OpenInterface()
{
	for (int i = 0; i < 1; i++) {
		PTZDevice *ptz = new PTZSimulator();
		ptz->setObjectName("PTZ Simulator");
	}

#if LIBVISCA_FOUND
	for (int i = 0; i < 3; i++) {
		PTZDevice *ptz = new PTZVisca("/dev/ttyUSB0", i+1);
		ptz->setObjectName("VISCA PTZ");
	}
#endif
}

void PTZControls::ControlContextMenu()
{

	/*
	QAction showTimeDecimalsAction(obs_module_text("ShowTimeDecimals"),
				       this);
	showTimeDecimalsAction.setCheckable(true);
	showTimeDecimalsAction.setChecked(showTimeDecimals);

	connect(&showTimeDecimalsAction, &QAction::toggled, this,
		&diaControls::ToggleShowTimeDecimals, Qt::DirectConnection);
	*/

	QMenu popup;
	/*
	popup.addAction(&showTimeDecimalsAction);
	*/
	popup.addSeparator();
	popup.exec(QCursor::pos());
}

PTZDevice * PTZControls::currCamera()
{
	if (PTZDevice::device_count() == 0)
		return NULL;
	if (current_cam >= PTZDevice::device_count())
		current_cam = PTZDevice::device_count() - 1;
	return PTZDevice::get_device(current_cam);
}

void PTZControls::setPanTilt(double pan, double tilt)
{
	PTZDevice *ptz = currCamera();
	if (!ptz)
		return;
	ptz->pantilt(pan * 10, tilt * 10);
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
	PTZDevice *ptz = currCamera();
	if (ptz)
		ptz->zoom_tele();
}
void PTZControls::on_zoomButton_tele_released()
{
	PTZDevice *ptz = currCamera();
	if (ptz)
		ptz->zoom_stop();
}
void PTZControls::on_zoomButton_wide_pressed()
{
	PTZDevice *ptz = currCamera();
	if (ptz)
		ptz->zoom_wide();
}
void PTZControls::on_zoomButton_wide_released()
{
	PTZDevice *ptz = currCamera();
	if (ptz)
		ptz->zoom_stop();
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
	ui->cameraList->setCurrentIndex(PTZDevice::model()->index(current_cam, 0));
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

void PTZControls::on_cameraList_clicked()
{
	full_stop();
	current_cam = ui->cameraList->currentIndex().row();
}
