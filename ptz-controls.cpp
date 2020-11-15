/* Pan Tilt Zoom camera controls
 *
 * Copyright 2020 Grant Likely <grant.likely@secretlab.ca>
 *
 * SPDX-License-Identifier: GPLv2
 */
#include "ptz-controls.hpp"
#include <QToolTip>

#include <obs-module.h>
#include <QMainWindow>
#include <QMenuBar>
#include <QVBoxLayout>
#include "ui_ptz-controls.h"

#include "../../item-widget-helpers.hpp"
#include "../../obs-app.hpp"

#if LIBVISCA_FOUND
#include <ptz-visca.hpp>
#endif

OBS_DECLARE_MODULE()
OBS_MODULE_AUTHOR("Grant Likely <grant.likely@secretlab.ca>");
OBS_MODULE_USE_DEFAULT_LOCALE("media-controls", "en-US")

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

/*
void PTZControls::OBSSignal(void *data, const char *signal,
			      calldata_t *call_data)
{
	obs_source_t *source =
		static_cast<obs_source_t *>(calldata_ptr(call_data, "source"));
	if (!source)
		return;
	uint32_t flags = obs_source_get_output_flags(source);
	if ((flags & OBS_SOURCE_CONTROLLABLE_MEDIA) == 0 ||
	    strcmp(signal, "source_destroy") == 0 ||
	    strcmp(signal, "source_remove") == 0)
		return;

	//PTZControls *controls = static_cast<PTZControls *>(data);
	//QMetaObject::invokeMethod(controls, "SignalMediaSource");
}
*/

/*
void PTZControls::OBSFrontendEvent(enum obs_frontend_event event, void *ptr)
{
	//PTZControls *controls = reinterpret_cast<PTZControls *>(ptr);

	switch (event) {
	default:
		break;
	}
}
*/

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

	//signal_handler_connect_global(obs_get_signal_handler(), OBSSignal, this);
	//obs_frontend_add_event_callback(OBSFrontendEvent, this);

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
	obs_data_erase(data, "cameras");
	obs_data_array_t *camera_array = obs_data_array_create();
	obs_data_set_array(data, "cameras", camera_array);
	for (unsigned long int i = 0; i < cameras.size(); i++) {
		PTZDevice *ptz = cameras.at(i);
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
	deleteLater();
}

void PTZControls::LoadConfig()
{
	char *file = obs_module_config_path("config.json");
	obs_data_t *data = nullptr;
	obs_data_array_t *array;

	if (file) {
		data = obs_data_create_from_json_file(file);
		bfree(file);
	}
	if (!data) {
		qDebug() << "No configuration data; creating test devices";
		OpenInterface();
		return;
	}

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
		if (!ptz)
			continue;
		ptz->setParent(this);
		cameras.push_back(ptz);
	}
}

void PTZControls::OpenInterface()
{
	for (int i = 0; i < 1; i++) {
		PTZDevice *ptz = new PTZSimulator();
		ptz->setParent(this);
		ptz->setObjectName("PTZ Simulator");
		cameras.push_back(ptz);
	}

#if LIBVISCA_FOUND
	for (int i = 0; i < 3; i++) {
		PTZDevice *ptz = new PTZVisca("/dev/ttyUSB0", i+1);
		ptz->setParent(this);
		ptz->setObjectName("VISCA PTZ");
		cameras.push_back(ptz);
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
	if (cameras.size() == 0)
		return NULL;
	if (current_cam >= cameras.size())
		current_cam = cameras.size() - 1;
	return cameras.at(current_cam);
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
	setPanTilt(gamepad->axisLeftX(), gamepad->axisLeftY());
}

/* The pan/tilt buttons are a large block of simple and mostly identical code.
 * Use C preprocessor macro to create all the duplicate functions */
#define button_pantilt_actions(direction) \
	void PTZControls::on_panTiltButton_##direction##_pressed() \
	{ PTZDevice *ptz = currCamera(); if (ptz) ptz->pantilt_##direction(10, 10); } \
	void PTZControls::on_panTiltButton_##direction##_released() \
	{ PTZDevice *ptz = currCamera(); if (ptz) ptz->pantilt_stop(); }

button_pantilt_actions(up);
button_pantilt_actions(upleft);
button_pantilt_actions(upright);
button_pantilt_actions(left);
button_pantilt_actions(right);
button_pantilt_actions(down);
button_pantilt_actions(downleft);
button_pantilt_actions(downright);

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

void PTZControls::on_nextCameraButton_released()
{
	full_stop();
	current_cam++;
	if (current_cam >= cameras.size())
		current_cam = 0;
}

void PTZControls::on_prevCameraButton_released()
{
	full_stop();
	current_cam--;
	if (current_cam >= cameras.size())
		current_cam = cameras.size() - 1;
}
