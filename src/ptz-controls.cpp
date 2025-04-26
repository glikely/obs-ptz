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
#include <QDockWidget>
#include <QLabel>

#include <qt-wrappers.hpp>
#include "touch-control.hpp"
#include "ui_ptz-controls.h"
#include "ptz-controls.hpp"
#include "settings.hpp"
#include "ptz.h"

void ptz_load_controls(void)
{
	const auto main_window = static_cast<QMainWindow *>(obs_frontend_get_main_window());
	obs_frontend_push_ui_translation(obs_module_get_string);
	auto *ctrls = new PTZControls(main_window);

	if (!obs_frontend_add_dock_by_id("ptz-dock", "PTZ Controls", ctrls))
		blog(LOG_ERROR, "failed to add PTZ controls dock");

	obs_frontend_pop_ui_translation();
}

PTZControls *PTZControls::instance = NULL;

/**
 * class buttonResizeFilter - Event filter to adjust button minimum height and resize icon
 *
 * This filter will update the minimumHeight property to keep a button square
 * when possible. It will also resize the button icon to match the button size.
 */
class squareResizeFilter : public QObject {
public:
	squareResizeFilter(QObject *parent) : QObject(parent) {}
	bool eventFilter(QObject *watched, QEvent *event) override
	{
		auto obj = qobject_cast<QWidget *>(watched);
		if (!obj || event->type() != QEvent::Resize)
			return false;
		auto resEvent = static_cast<QResizeEvent *>(event);

		obj->setMinimumHeight(resEvent->size().width());

		auto button = qobject_cast<QAbstractButton *>(watched);
		if (button) {
			int size = resEvent->size().width() * 2 / 3;
			button->setIconSize(QSize(size, size));
		}
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

	switch (event) {
	case OBS_FRONTEND_EVENT_TRANSITION_STOPPED:
		updateMoveControls();
		break;
	case OBS_FRONTEND_EVENT_SCENE_CHANGED:
		if (ui->actionFollowProgram->isChecked())
			scene = obs_frontend_get_current_scene();
		updateMoveControls();
		break;
	case OBS_FRONTEND_EVENT_STUDIO_MODE_ENABLED:
	case OBS_FRONTEND_EVENT_STUDIO_MODE_DISABLED:
	case OBS_FRONTEND_EVENT_PREVIEW_SCENE_CHANGED:
		if (ui->actionFollowPreview->isChecked())
			scene = obs_frontend_get_current_preview_scene();
		updateMoveControls();
		break;
	default:
		break;
	}

	if (!scene)
		return;

	struct active_src_cb_data {
		PTZDevice *ptz;
	};
	auto active_src_cb = [](obs_source_t *parent, obs_source_t *child, void *context_data) {
		Q_UNUSED(parent);
		struct active_src_cb_data *context = static_cast<struct active_src_cb_data *>(context_data);
		if (!context->ptz)
			context->ptz = ptzDeviceList.getDeviceByName(obs_source_get_name(child));
	};
	struct active_src_cb_data cb_data;
	cb_data.ptz = ptzDeviceList.getDeviceByName(obs_source_get_name(scene));
	if (!cb_data.ptz)
		obs_source_enum_active_sources(scene, active_src_cb, &cb_data);
	obs_source_release(scene);

	if (cb_data.ptz)
		setCurrent(cb_data.ptz->getId());
}

PTZControls::PTZControls(QWidget *parent) : QWidget(parent), ui(new Ui::PTZControls)
{
	instance = this;
	ui->setupUi(this);
	ui->cameraList->setModel(&ptzDeviceList);
	ui->cameraList->setItemDelegate(new PTZDeviceListDelegate(ui->cameraList));
	connect(&ptzDeviceList, SIGNAL(dataChanged(QModelIndex,QModelIndex)), this, SLOT(ptzDeviceDataChanged(const QModelIndex&, const QModelIndex&)));

	copyActionsDynamicProperties();

	QItemSelectionModel *selectionModel = ui->cameraList->selectionModel();
	connect(selectionModel, SIGNAL(currentChanged(QModelIndex, QModelIndex)), this,
		SLOT(currentChanged(QModelIndex, QModelIndex)));
	connect(&accel_timer, &QTimer::timeout, this, &PTZControls::accelTimerHandler);

	connect(ui->panTiltTouch, SIGNAL(positionChanged(double, double)), this, SLOT(setPanTilt(double, double)));

	joystickSetup();

	LoadConfig();

	auto filter = new squareResizeFilter(this);
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
	ui->panTiltTouch->installEventFilter(filter);

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
	auto registerHotkey = [&](const char *name, const char *description, obs_hotkey_func func,
				  void *hotkey_data) -> obs_hotkey_id {
		obs_hotkey_id id;

		id = obs_hotkey_register_frontend(name, description, func, hotkey_data);
		OBSDataArrayAutoRelease array = obs_data_get_array(loadHotkeyData(name), "bindings");
		obs_hotkey_load(id, array);
		hotkeys << id;
		return id;
	};
	auto cb = [](void *button_data, obs_hotkey_id, obs_hotkey_t *, bool pressed) {
		QPushButton *button = static_cast<QPushButton *>(button_data);
		if (pressed)
			button->pressed();
		else
			button->released();
	};
	auto prevcb = [](void *action_data, obs_hotkey_id, obs_hotkey_t *, bool pressed) {
		auto list = static_cast<CircularListView *>(action_data);
		if (pressed)
			list->cursorUp();
	};
	auto nextcb = [](void *action_data, obs_hotkey_id, obs_hotkey_t *, bool pressed) {
		auto list = static_cast<CircularListView *>(action_data);
		if (pressed)
			list->cursorDown();
	};
	auto actiontogglecb = [](void *action_data, obs_hotkey_id, obs_hotkey_t *, bool pressed) {
		QAction *action = static_cast<QAction *>(action_data);
		if (pressed)
			action->activate(QAction::Trigger);
	};
	auto autofocustogglecb = [](void *ptz_data, obs_hotkey_id, obs_hotkey_t *, bool pressed) {
		PTZControls *ptzctrl = static_cast<PTZControls *>(ptz_data);
		if (pressed)
			ptzctrl->on_focusButton_auto_clicked(!ptzctrl->ui->focusButton_auto->isChecked());
	};
	registerHotkey("PTZ.PanTiltUpLeft", "PTZ Pan camera up & left", cb, ui->panTiltButton_upleft);
	registerHotkey("PTZ.PanTiltLeft", "PTZ Pan camera left", cb, ui->panTiltButton_left);
	registerHotkey("PTZ.PanTiltDownLeft", "PTZ Pan camera down & left", cb, ui->panTiltButton_downleft);
	registerHotkey("PTZ.PanTiltUpRight", "PTZ Pan camera up & right", cb, ui->panTiltButton_upright);
	registerHotkey("PTZ.PanTiltRight", "PTZ Pan camera right", cb, ui->panTiltButton_right);
	registerHotkey("PTZ.PanTiltDownRight", "PTZ Pan camera down & right", cb, ui->panTiltButton_downright);
	registerHotkey("PTZ.PanTiltUp", "PTZ Tilt camera up", cb, ui->panTiltButton_up);
	registerHotkey("PTZ.PanTiltDown", "PTZ Tilt camera down", cb, ui->panTiltButton_down);
	registerHotkey("PTZ.ZoomWide", "PTZ Zoom camera out (wide)", cb, ui->zoomButton_wide);
	registerHotkey("PTZ.ZoomTele", "PTZ Zoom camera in (telefocal)", cb, ui->zoomButton_tele);
	registerHotkey("PTZ.FocusAutoFocus", "PTZ Toggle Autofocus", autofocustogglecb, this);
	registerHotkey("PTZ.FocusNear", "PTZ Focus far", cb, ui->focusButton_far);
	registerHotkey("PTZ.FocusFar", "PTZ Focus near", cb, ui->focusButton_near);
	registerHotkey("PTZ.FocusOneTouch", "PTZ One touch focus trigger", cb, ui->focusButton_onetouch);
	registerHotkey("PTZ.SelectPrev", "PTZ Select previous device in list", prevcb, ui->cameraList);
	registerHotkey("PTZ.SelectNext", "PTZ Select next device in list", nextcb, ui->cameraList);
	registerHotkey("PTZ.ActionFollowPreviewToggle", "PTZ Autoselect Preview Camera", actiontogglecb,
		       ui->actionFollowPreview);
	registerHotkey("PTZ.ActionFollowProgramToggle", "PTZ Autoselect Live Camera", actiontogglecb,
		       ui->actionFollowProgram);

	auto preset_recall_cb = [](void *ptz_data, obs_hotkey_id hotkey, obs_hotkey_t *, bool pressed) {
		PTZControls *ptzctrl = static_cast<PTZControls *>(ptz_data);
		auto id = ptzctrl->preset_hotkey_map[hotkey];
		if (pressed)
			ptzctrl->presetRecall(id);
	};

	auto preset_set_cb = [](void *ptz_data, obs_hotkey_id hotkey, obs_hotkey_t *, bool pressed) {
		PTZControls *ptzctrl = static_cast<PTZControls *>(ptz_data);
		auto id = ptzctrl->preset_hotkey_map[hotkey];
		if (pressed)
			ptzctrl->presetSet(id);
	};

	for (int i = 0; i < 16; i++) {
		auto name = QString("PTZ.Recall%1").arg(i + 1);
		auto description = QString("PTZ Memory Recall #%1").arg(i + 1);
		auto hotkey = registerHotkey(QT_TO_UTF8(name), QT_TO_UTF8(description), preset_recall_cb, this);
		preset_hotkey_map[hotkey] = i;
		name = QString("PTZ.Save%1").arg(i + 1);
		description = QString("PTZ Memory Save #%1").arg(i + 1);
		hotkey = registerHotkey(QT_TO_UTF8(name), QT_TO_UTF8(description), preset_set_cb, this);
		preset_hotkey_map[hotkey] = i;
	}
}

PTZControls::~PTZControls()
{
	while (!hotkeys.isEmpty())
		obs_hotkey_unregister(hotkeys.takeFirst());

	SaveConfig();
	ptzDeviceList.delete_all();
	deleteLater();
}

#if defined(ENABLE_JOYSTICK)
void PTZControls::joystickSetup()
{
	auto joysticks = QJoysticks::getInstance();
	joysticks->setVirtualJoystickEnabled(false);
	joysticks->updateInterfaces();
	connect(joysticks, SIGNAL(axisEvent(const QJoystickAxisEvent)), this,
		SLOT(joystickAxisEvent(const QJoystickAxisEvent)));
	connect(joysticks, SIGNAL(buttonEvent(const QJoystickButtonEvent)), this,
		SLOT(joystickButtonEvent(const QJoystickButtonEvent)));
	connect(joysticks, SIGNAL(POVEvent(const QJoystickPOVEvent)), this,
		SLOT(joystickPOVEvent(const QJoystickPOVEvent)));
}

void PTZControls::setJoystickEnabled(bool enable)
{
	/* Stop camera on state change */
	setPanTilt(0, 0);
	setZoom(0);
	m_joystick_enable = enable;
}

void PTZControls::setJoystickSpeed(double speed)
{
	m_joystick_speed = speed;
	/* Immediatly apply the deadzone */
	auto jd = QJoysticks::getInstance()->getInputDevice(m_joystick_id);
	joystickAxesChanged(jd, 0b11111111);
}

void PTZControls::setJoystickDeadzone(double deadzone)
{
	m_joystick_deadzone = deadzone;
	/* Immediatly apply the deadzone */
	auto jd = QJoysticks::getInstance()->getInputDevice(m_joystick_id);
	joystickAxesChanged(jd, 0b11111111);
}

void PTZControls::joystickAxesChanged(const QJoystickDevice *jd, uint32_t updated)
{
	if (!m_joystick_enable || !jd || jd->id != m_joystick_id)
		return;
	auto filter_axis = [=](double val) -> double {
		val = abs(val) > m_joystick_deadzone ? val : 0.0;
		return val * m_joystick_speed;
	};

	if (updated & 0b0011 && jd->axes.size() > 1)
		setPanTilt(filter_axis(jd->axes[0]), -filter_axis(jd->axes[1]));
	if (updated & 0b1000 && jd->axes.size() > 3)
		setZoom(-filter_axis(jd->axes[3]));
}

void PTZControls::joystickAxisEvent(const QJoystickAxisEvent evt)
{
	joystickAxesChanged(evt.joystick, 1 << evt.axis);
}

void PTZControls::joystickPOVEvent(const QJoystickPOVEvent evt)
{
	if (!m_joystick_enable || evt.joystick->id != m_joystick_id)
		return;
	switch (evt.angle) {
	case 0:
		ui->presetListView->cursorUp();
		break;
	case 180:
		ui->presetListView->cursorDown();
		break;
	}
}

void PTZControls::joystickButtonEvent(const QJoystickButtonEvent evt)
{
	QModelIndex index;
	if (!m_joystick_enable || evt.joystick->id != m_joystick_id)
		return;
	if (!evt.pressed)
		return;
	switch (evt.button) {
	case 0: /* A button; activate preset */
		index = ui->presetListView->currentIndex();
		if (index.isValid())
			ui->presetListView->activated(index);
		break;
	case 4: /* left shoulder; previous camera */
		ui->cameraList->cursorUp();
		break;
	case 5: /* right shoulder; next camera */
		ui->cameraList->cursorDown();
		break;
	}
}
#endif /* ENABLE_JOYSTICK */

void PTZControls::copyActionsDynamicProperties()
{
	// Themes need the QAction dynamic properties
	for (QAction *x : ui->ptzToolbar->actions()) {
		QWidget *temp = ui->ptzToolbar->widgetForAction(x);

		for (QByteArray &y : x->dynamicPropertyNames()) {
			temp->setProperty(y, x->property(y));
		}
	}
}

/*
 * Save/Load configuration methods
 */
void PTZControls::SaveConfig()
{
	char *file = obs_module_config_path("config.json");
	if (!file)
		return;

	OBSData savedata = obs_data_create();
	obs_data_release(savedata);

	obs_data_set_string(savedata, "splitter_state", ui->splitter->saveState().toBase64().constData());

	obs_data_set_bool(savedata, "live_moves_disabled", live_moves_disabled);
	obs_data_set_int(savedata, "debug_log_level", ptz_debug_level);
	const char *target_mode = "manual";
	if (ui->actionFollowPreview->isChecked())
		target_mode = "preview";
	if (ui->actionFollowProgram->isChecked())
		target_mode = "program";
	obs_data_set_string(savedata, "target_mode", target_mode);
	obs_data_set_bool(savedata, "joystick_enable", m_joystick_enable);
	obs_data_set_int(savedata, "joystick_id", m_joystick_id);
	obs_data_set_double(savedata, "joystick_speed", m_joystick_speed);
	obs_data_set_double(savedata, "joystick_deadzone", m_joystick_deadzone);

	OBSDataArray camera_array = ptz_devices_get_config();
	obs_data_array_release(camera_array);
	obs_data_set_array(savedata, "devices", camera_array);

	/* Save data structure to json */
	if (!obs_data_save_json_safe(savedata, file, "tmp", "bak")) {
		char *path = obs_module_config_path("");
		if (path) {
			os_mkdirs(path);
			bfree(path);
		}
		obs_data_save_json_safe(savedata, file, "tmp", "bak");
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

	OBSData loaddata = obs_data_create_from_json_file_safe(file, "bak");
	if (!loaddata) {
		/* Try loading from the old configuration path */
		auto f = QString(file).replace("obs-ptz", "ptz-controls");
		loaddata = obs_data_create_from_json_file_safe(QT_TO_UTF8(f), "bak");
	}
	bfree(file);
	if (!loaddata)
		return;
	obs_data_release(loaddata);
	obs_data_set_default_int(loaddata, "current_speed", 50);
	obs_data_set_default_int(loaddata, "debug_log_level", LOG_INFO);
	obs_data_set_default_bool(loaddata, "live_moves_disabled", true);
	obs_data_set_default_string(loaddata, "target_mode", "preview");
	obs_data_set_default_bool(loaddata, "joystick_enable", false);
	obs_data_set_default_int(loaddata, "joystick_id", -1);
	obs_data_set_default_double(loaddata, "joystick_speed", 1.0);
	obs_data_set_default_double(loaddata, "joystick_deadzone", 0.0);

	ptz_debug_level = (int)obs_data_get_int(loaddata, "debug_log_level");
	live_moves_disabled = obs_data_get_bool(loaddata, "live_moves_disabled");
	target_mode = obs_data_get_string(loaddata, "target_mode");
	ui->actionFollowPreview->setChecked(target_mode == "preview");
	ui->actionFollowProgram->setChecked(target_mode == "program");
	m_joystick_enable = obs_data_get_bool(loaddata, "joystick_enable");
	m_joystick_id = (int)obs_data_get_int(loaddata, "joystick_id");
	m_joystick_speed = obs_data_get_double(loaddata, "joystick_speed");
	m_joystick_deadzone = obs_data_get_double(loaddata, "joystick_deadzone");

	const char *splitterStateStr = obs_data_get_string(loaddata, "splitter_state");
	if (splitterStateStr) {
		QByteArray splitterState = QByteArray::fromBase64(QByteArray(splitterStateStr));
		ui->splitter->restoreState(splitterState);
	}

	array = obs_data_get_array(loaddata, "devices");
	obs_data_array_release(array);
	ptz_devices_set_config(array);
}

void PTZControls::setDisableLiveMoves(bool disable)
{
	live_moves_disabled = disable;
	updateMoveControls();
}

PTZDevice *PTZControls::currCamera()
{
	return ptzDeviceList.getDevice(ui->cameraList->currentIndex());
}

void PTZControls::accelTimerHandler()
{
	PTZDevice *ptz = currCamera();
	if (!ptz) {
		accel_timer.stop();
		return;
	}

	pan_speed = std::clamp(pan_speed + pan_accel, -1.0, 1.0);
	if (std::abs(pan_speed) == 1.0)
		pan_accel = 0.0;
	tilt_speed = std::clamp(tilt_speed + tilt_accel, -1.0, 1.0);
	if (std::abs(tilt_speed) == 1.0)
		tilt_accel = 0.0;
	ptz->pantilt(pan_speed, tilt_speed);
	zoom_speed = std::clamp(zoom_speed + zoom_accel, -1.0, 1.0);
	if (std::abs(zoom_speed) == 1.0)
		zoom_accel = 0.0;
	ptz->zoom(zoom_speed);
	focus_speed = std::clamp(focus_speed + focus_accel, -1.0, 1.0);
	if (std::abs(focus_speed) == 1.0)
		focus_accel = 0.0;
	ptz->focus(focus_speed);
	if (pan_accel == 0.0 && tilt_accel == 0.0 && zoom_accel == 0.0 && focus_accel == 0.0)
		accel_timer.stop();
}

void PTZControls::setPanTilt(double pan, double tilt, double pan_accel_, double tilt_accel_)
{
	pan_speed = pan;
	tilt_speed = tilt;
	pan_accel = pan_accel_;
	tilt_accel = tilt_accel_;
	pantiltingFlag = pan != 0 || tilt != 0;

	PTZDevice *ptz = currCamera();
	if (!ptz)
		return;
	ptz->pantilt(pan, tilt);
	if (pan_accel != 0 || tilt_accel != 0)
		accel_timer.start(2000 / 20);
}

void PTZControls::keypressPanTilt(double pan, double tilt)
{
	auto modifiers = QGuiApplication::keyboardModifiers();
	if (modifiers.testFlag(Qt::ControlModifier))
		setPanTilt(pan, tilt);
	else if (modifiers.testFlag(Qt::ShiftModifier))
		setPanTilt(pan / 20, tilt / 20);
	else
		setPanTilt(pan / 20, tilt / 20, pan / 20, tilt / 20);
}

/** setZoom(double speed)
 *
 * Direction:
 *   speed < 0: Zoom out (wide)
 *   speed = 0: Stop Zooming
 *   speed > 0: Zoom in (tele)
 */
void PTZControls::setZoom(double zoom)
{
	PTZDevice *ptz = currCamera();
	if (!ptz)
		return;

	zoomingFlag = (zoom != 0.0);
	if (QGuiApplication::keyboardModifiers().testFlag(Qt::ControlModifier)) {
		ptz->zoom(zoom);
	} else if (QGuiApplication::keyboardModifiers().testFlag(Qt::ShiftModifier)) {
		ptz->zoom(zoom / 20);
	} else {
		zoom_speed = zoom_accel = zoom / 20;
		ptz->zoom(zoom_speed);
		accel_timer.start(2000 / 20);
	}
}

void PTZControls::setFocus(double focus)
{
	PTZDevice *ptz = currCamera();
	if (!ptz)
		return;

	focusingFlag = (focus != 0.0);
	if (QGuiApplication::keyboardModifiers().testFlag(Qt::ControlModifier)) {
		ptz->focus(focus);
	} else if (QGuiApplication::keyboardModifiers().testFlag(Qt::ShiftModifier)) {
		ptz->focus(focus / 20);
	} else {
		focus_speed = focus_accel = focus / 20;
		ptz->focus(focus_speed);
		accel_timer.start(2000 / 20);
	}
}

/* The pan/tilt buttons are a large block of simple and mostly identical code.
 * Use C preprocessor macro to create all the duplicate functions */
#define button_pantilt_actions(direction, x, y)                     \
	void PTZControls::on_panTiltButton_##direction##_pressed()  \
	{                                                           \
		keypressPanTilt(x, y);                              \
	}                                                           \
	void PTZControls::on_panTiltButton_##direction##_released() \
	{                                                           \
		keypressPanTilt(0, 0);                              \
	}

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
	setZoom(1);
}

void PTZControls::on_zoomButton_tele_released()
{
	setZoom(0);
}

void PTZControls::on_zoomButton_wide_pressed()
{
	setZoom(-1);
}

void PTZControls::on_zoomButton_wide_released()
{
	setZoom(0);
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
	setFocus(1);
}

void PTZControls::on_focusButton_near_released()
{
	setFocus(0);
}

void PTZControls::on_focusButton_far_pressed()
{
	setFocus(-1);
}

void PTZControls::on_focusButton_far_released()
{
	setFocus(0);
}

void PTZControls::on_focusButton_onetouch_clicked()
{
	PTZDevice *ptz = currCamera();
	if (ptz)
		ptz->focus_onetouch();
}

void PTZControls::setCurrent(uint32_t device_id)
{
	if (device_id == ptzDeviceList.getDeviceId(ui->cameraList->currentIndex()))
		return;
	ui->cameraList->setCurrentIndex(ptzDeviceList.indexFromDeviceId(device_id));
}

void PTZControls::on_actionFollowPreview_toggled(bool checked)
{
	if (checked)
		OBSFrontendEvent(OBS_FRONTEND_EVENT_PREVIEW_SCENE_CHANGED);
}

void PTZControls::on_actionFollowProgram_toggled(bool checked)
{
	if (checked)
		OBSFrontendEvent(OBS_FRONTEND_EVENT_SCENE_CHANGED);
}

void PTZControls::setAutofocusEnabled(bool autofocus_on)
{
	ui->focusButton_auto->setChecked(autofocus_on);
	ui->focusButton_near->setEnabled(!autofocus_on);
	ui->focusButton_far->setEnabled(!autofocus_on);
	ui->focusButton_onetouch->setEnabled(!autofocus_on);
}

void PTZControls::ptzDeviceDataChanged(const QModelIndex &, const QModelIndex &)
{
	int rows = ptzDeviceList.rowCount();
	for (int i = 0; i < rows; i++) {
		auto index = ptzDeviceList.index(i, 0);
		auto ptzitem = reinterpret_cast<PTZDeviceListItem *>(ui->cameraList->indexWidget(index));
		if (ptzitem)
			ptzitem->update();
		else {
			auto ptz_ = ptzDeviceList.getDevice(index);
			ui->cameraList->setIndexWidget(index, new PTZDeviceListItem(ptz_));
		}
	}
}

void PTZControls::updateMoveControls()
{
	bool ctrls_enabled = true;
	PTZDevice *ptz = currCamera();

	int rows = ptzDeviceList.rowCount();
	for (int i = 0; i < rows; i++) {
		auto index = ptzDeviceList.index(i, 0);
		auto ptzitem = reinterpret_cast<PTZDeviceListItem *>(ui->cameraList->indexWidget(index));
		if (ptzitem)
			ptzitem->update();
		else {
			auto ptz_ = ptzDeviceList.getDevice(index);
			ui->cameraList->setIndexWidget(index, new PTZDeviceListItem(ptz_));
		}
	}

	// Check if the device's source is in the active program scene
	// If it is then disable the pan/tilt/zoom controls
	auto item = ui->cameraList->indexWidget(ui->cameraList->currentIndex());
	auto ptzitem = reinterpret_cast<PTZDeviceListItem *>(item);
	if (obs_frontend_preview_program_mode_active() && live_moves_disabled && ptz)
		ctrls_enabled = !ptzitem->isLocked();

	ui->movementControlsWidget->setEnabled(ctrls_enabled);
	ui->presetListView->setEnabled(ctrls_enabled);

	ui->actionFollowPreview->setVisible(obs_frontend_preview_program_mode_active());
	RefreshToolBarStyling(ui->ptzToolbar);
}

void PTZControls::currentChanged(QModelIndex current, QModelIndex previous)
{
	PTZDevice *ptz = ptzDeviceList.getDevice(previous);
	accel_timer.stop();
	if (ptz) {
		disconnect(ptz, nullptr, this, nullptr);
		if (pantiltingFlag)
			ptz->pantilt(0, 0);
		if (zoomingFlag)
			ptz->zoom(0);
		if (focusingFlag)
			ptz->focus(0);
	}
	pantiltingFlag = false;
	zoomingFlag = false;
	focusingFlag = false;
	pan_speed = pan_accel = 0.0;
	tilt_speed = tilt_accel = 0.0;
	zoom_speed = zoom_accel = 0.0;
	focus_speed = focus_accel = 0.0;

	ptz = ptzDeviceList.getDevice(current);
	if (ptz) {
		ui->presetListView->setModel(ptz->presetModel());
		presetUpdateActions();
		auto *selectionModel = ui->presetListView->selectionModel();
		if (selectionModel)
			connect(selectionModel, SIGNAL(currentChanged(QModelIndex, QModelIndex)), this,
				SLOT(presetUpdateActions()));
		ptz->connect(ptz, SIGNAL(settingsChanged(OBSData)), this, SLOT(settingsChanged(OBSData)));

		auto settings = ptz->get_settings();
		setAutofocusEnabled(obs_data_get_bool(settings, "focus_af_enabled"));
	}

	updateMoveControls();
}

void PTZControls::settingsChanged(OBSData settings)
{
	if (obs_data_has_user_value(settings, "focus_af_enabled"))
		setAutofocusEnabled(obs_data_get_bool(settings, "focus_af_enabled"));
}

void PTZControls::presetSet(int preset_id)
{
	PTZDevice *ptz = currCamera();
	if (!ptz)
		return;
	ptz->memory_set(preset_id);
}

void PTZControls::presetRecall(int preset_id)
{
	PTZDevice *ptz = currCamera();
	if (!ptz)
		return;
	ptz->memory_recall(preset_id);
}

int PTZControls::presetIndexToId(QModelIndex index)
{
	auto model = ui->presetListView->model();
	if (model && index.isValid())
		return model->data(index, Qt::UserRole).toInt();
	return -1;
}

void PTZControls::presetUpdateActions()
{
	auto index = ui->presetListView->currentIndex();
	auto model = ui->presetListView->model();
	int count = model ? model->rowCount() : 0;
	ui->actionPresetAdd->setEnabled(model != nullptr);
	ui->actionPresetRemove->setEnabled(index.isValid());
	ui->actionPresetMoveUp->setEnabled(index.isValid() && count > 1 && index.row() > 0);
	ui->actionPresetMoveDown->setEnabled(index.isValid() && count > 1 && index.row() < count - 1);
	RefreshToolBarStyling(ui->ptzToolbar);
}

void PTZControls::on_presetListView_activated(QModelIndex index)
{
	presetRecall(presetIndexToId(index));
}

void PTZControls::on_pantiltStack_customContextMenuRequested(const QPoint &pos)
{
	QPoint globalpos = ui->pantiltStack->mapToGlobal(pos);
	QMenu menu;
	bool enabled = (ui->pantiltStack->currentIndex() != 0);

	QAction *touchpadAction = menu.addAction("Onscreen Joystick");
	touchpadAction->setCheckable(true);
	touchpadAction->setChecked(enabled);
	QAction *action = menu.exec(globalpos);
	if (action == nullptr)
		return;

	if (action == touchpadAction)
		ui->pantiltStack->setCurrentIndex(!enabled ? 1 : 0);
}

void PTZControls::on_presetListView_customContextMenuRequested(const QPoint &pos)
{
	QPoint globalpos = ui->presetListView->mapToGlobal(pos);
	QModelIndex index = ui->presetListView->indexAt(pos);
	PTZDevice *ptz = currCamera();
	if (!ptz)
		return;

	QMenu presetContext;
	QAction *renameAction = presetContext.addAction("Rename");
	QAction *setAction = presetContext.addAction("Save Preset");
	QAction *resetAction = presetContext.addAction("Clear Preset");
	presetContext.addAction(ui->actionPresetAdd);
	if (index.isValid())
		presetContext.addAction(ui->actionPresetRemove);
	QAction *action = presetContext.exec(globalpos);

	if (action == renameAction) {
		ui->presetListView->edit(index);
	} else if (action == setAction) {
		ptz->memory_set(presetIndexToId(index));
	} else if (action == resetAction) {
		ptz->memory_reset(presetIndexToId(index));
		ui->presetListView->model()->setData(index, "");
	}
}

void PTZControls::on_cameraList_doubleClicked(const QModelIndex &index)
{
	ptz_settings_show(ptzDeviceList.getDeviceId(index));
}

void PTZControls::on_cameraList_customContextMenuRequested(const QPoint &pos)
{
	QPoint globalpos = ui->cameraList->mapToGlobal(pos);
	QModelIndex index = ui->cameraList->indexAt(pos);
	PTZDevice *ptz = ptzDeviceList.getDevice(index);
	QMenu context;
	QAction *powerAction = nullptr;
	QAction *wbOnetouchAction = nullptr;

	if (ptz) {
		OBSData settings = ptz->get_settings();
		bool power_on = obs_data_get_bool(settings, "power_on");
		powerAction = context.addAction(power_on ? "Power Off" : "Power On");

		bool wb_onepush = (obs_data_get_int(settings, "wb_mode") == 3);
		if (wb_onepush)
			wbOnetouchAction = context.addAction("Trigger One-Push White Balance");
	}
	QAction *propertiesAction = context.addAction("Properties");
	QAction *action = context.exec(globalpos);

	OBSData setdata = obs_data_create();
	obs_data_release(setdata);

	if (action == nullptr)
		return;
	if (action == powerAction) {
		OBSData settings = ptz->get_settings();
		obs_data_set_bool(setdata, "power_on", !obs_data_get_bool(settings, "power_on"));
		ptz->set_settings(setdata);
	} else if (action == wbOnetouchAction) {
		obs_data_set_bool(setdata, "wb_onepush_trigger", true);
		ptz->set_settings(setdata);
	} else if (action == propertiesAction) {
		ptz_settings_show(ptzDeviceList.getDeviceId(ui->cameraList->currentIndex()));
	}
}

void PTZControls::on_actionPresetAdd_triggered()
{
	auto model = ui->presetListView->model();
	if (!model)
		return;
	auto row = model->rowCount();
	model->insertRows(row, 1);
	QModelIndex index = model->index(row, 0);
	if (index.isValid()) {
		ui->presetListView->setCurrentIndex(index);
		ui->presetListView->edit(index);
	}
	presetUpdateActions();
}

void PTZControls::on_actionPresetRemove_triggered()
{
	auto model = ui->presetListView->model();
	auto index = ui->presetListView->currentIndex();
	if (!model || !index.isValid())
		return;
	model->removeRows(index.row(), 1);
	presetUpdateActions();
}

void PTZControls::on_actionPresetMoveUp_triggered()
{
	auto model = ui->presetListView->model();
	auto index = ui->presetListView->currentIndex();
	if (!model || !index.isValid())
		return;
	model->moveRow(QModelIndex(), index.row(), QModelIndex(), index.row() - 1);
	presetUpdateActions();
}

void PTZControls::on_actionPresetMoveDown_triggered()
{
	auto model = ui->presetListView->model();
	auto index = ui->presetListView->currentIndex();
	if (!model || !index.isValid())
		return;
	model->moveRow(QModelIndex(), index.row(), QModelIndex(), index.row() + 2);
	presetUpdateActions();
}

PTZDeviceListDelegate::PTZDeviceListDelegate(QObject *parent) : QStyledItemDelegate(parent) {}

QSize PTZDeviceListDelegate::sizeHint(const QStyleOptionViewItem &option, const QModelIndex &index) const
{
	QListView *tree = qobject_cast<QListView *>(parent());
	QWidget *item = tree->indexWidget(index);

	if (!item)
		return (QSize(0, 0));

	return (QSize(option.widget->minimumWidth(), item->height()));
}

void PTZDeviceListDelegate::initStyleOption(QStyleOptionViewItem *option, const QModelIndex &) const
{
	option->text = QString();
}

PTZDeviceListItem::PTZDeviceListItem(PTZDevice *ptz_) : ptz(ptz_)
{
	setAttribute(Qt::WA_TranslucentBackground);
	lock = new QCheckBox();
	lock->setProperty("class", "checkbox-icon indicator-lock");
	lock->setChecked(ptz->isLive());
	lock->setAccessibleName("PTZ.Device.Lock");
	lock->setAccessibleDescription("PTZ.Device.LockDescription");

	label = new QLabel(ptz->objectName());

	boxLayout = new QHBoxLayout();
	boxLayout->setContentsMargins(0, 0, 0, 0);
	boxLayout->setSpacing(0);
	boxLayout->addWidget(label);
	boxLayout->addWidget(lock);
	setLayout(boxLayout);

	connect(lock, SIGNAL(clicked(bool)), PTZControls::getInstance(), SLOT(updateMoveControls()));

	update();
}

void PTZDeviceListItem::update()
{
	bool is_live = ptz->isLive();
	// When a camera becomes live, start with it locked
	if (lock->isVisible() == false && is_live)
		lock->setChecked(true);
	if (label->text() != ptz->objectName())
		label->setText(ptz->objectName());
	lock->setVisible(is_live);
}
