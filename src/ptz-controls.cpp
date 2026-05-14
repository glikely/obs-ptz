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
#include <QStylePainter>

#include <qt-wrappers.hpp>
#include "touch-control.hpp"
#include "ui_ptz-controls.h"
#include "ptz-controls.hpp"
#include "settings.hpp"
#include "ptz.h"

const char *ptz_joy_action_axis_names[PTZ_JOY_ACTION_LAST_VALUE] = {"None",
								    "Pan",
								    "Pan (Inverted)",
								    "Tilt",
								    "Tilt (Inverted)",
								    "Zoom",
								    "Zoom (Inverted)",
								    "Focus",
								    "Focus (Inverted)"};

void ptz_load_controls(void)
{
	const auto main_window = static_cast<QMainWindow *>(obs_frontend_get_main_window());
	obs_frontend_push_ui_translation(obs_module_get_string);
	auto *ctrls = new PTZControls(main_window);

	if (!obs_frontend_add_dock_by_id("ptz-dock", obs_module_text("PTZ.Dock.Name"), ctrls))
		blog(LOG_ERROR, "failed to add PTZ controls dock");

	obs_frontend_pop_ui_translation();
}

PTZControls *PTZControls::instance = NULL;

/**
 * class buttonResizeFilter - Event filter to adjust button minimum height and resize icon
 *
 * This filter will update the minimumHeight property to keep a button square
 * when possible.
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
		return true;
	}
};

void PTZControls::OBSFrontendEventWrapper(enum obs_frontend_event event, void *ptr)
{
	PTZControls *controls = reinterpret_cast<PTZControls *>(ptr);
	QMetaObject::invokeMethod(
		controls, [controls, event]() { controls->OBSFrontendEvent(event); }, Qt::QueuedConnection);
}

void PTZControls::OBSFrontendEvent(enum obs_frontend_event event)
{
	obs_source_t *scene = NULL;

	switch (event) {
	case OBS_FRONTEND_EVENT_TRANSITION_STOPPED:
		updateMoveControls();
		break;
	case OBS_FRONTEND_EVENT_SCENE_CHANGED:
		if (autoselectEnabled() && !obs_frontend_preview_program_mode_active())
			scene = obs_frontend_get_current_scene();

		/* The main scene has changed. Iterate over all the devices and mark the live
		 * ones as locked. The user can manually unlock a device when they want to
		 * perform a move on a live camera. */
		for (int i = 0; i < ptzDeviceList.rowCount(); i++) {
			auto index = ptzDeviceList.index(i, 0);
			bool isLive = index.data(PTZListModel::IsLiveRole).toBool();
			ptzDeviceList.setData(index, isLive, PTZListModel::IsLockedRole);
		}

		updateMoveControls();
		break;
	case OBS_FRONTEND_EVENT_STUDIO_MODE_ENABLED:
	case OBS_FRONTEND_EVENT_STUDIO_MODE_DISABLED:
	case OBS_FRONTEND_EVENT_PREVIEW_SCENE_CHANGED:
		if (autoselectEnabled() && obs_frontend_preview_program_mode_active())
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

/* Helper funciton for changing currently selected OBS scene */
static void change_current_scene(int delta)
{
	struct obs_frontend_source_list scenes = {0};
	obs_source_t *cur = obs_frontend_preview_program_mode_active() ? obs_frontend_get_current_preview_scene()
								       : obs_frontend_get_current_scene();
	if (!cur)
		return;
	obs_frontend_get_scenes(&scenes);
	size_t start = delta < 0 ? -delta : 0;
	size_t end = scenes.sources.num - (delta > 0 ? delta : 0);
	for (size_t i = start; i < end; i++) {
		if (cur == scenes.sources.array[i]) {
			if (obs_frontend_preview_program_mode_active()) {
				obs_frontend_set_current_preview_scene(scenes.sources.array[i + delta]);
			} else {
				obs_frontend_set_current_scene(scenes.sources.array[i + delta]);
			}
		}
	}
	obs_frontend_source_list_free(&scenes);
	obs_source_release(cur);
}

PTZControls::PTZControls(QWidget *parent) : QFrame(parent), ui(new Ui::PTZControls)
{
	instance = this;
	ui->setupUi(this);

	/* Compatability: Before OBS Studio 31.1.0 the theme had left and right
	 * margins on widgets which mess with the grid layout used by this
	 * plugin. If the version is earlier than 31.1.0 then apply an extra
	 * style sheet to fix */
	if (obs_get_version() < MAKE_SEMANTIC_VERSION(31, 1, 0))
		this->setStyleSheet("margin-left: 0px; margin-right: 0px");

	ui->cameraList->setModel(&ptzDeviceList);
	ui->cameraList->setItemDelegate(new PTZDeviceListDelegate(ui->cameraList));
	connect(&ptzDeviceList, SIGNAL(dataChanged(QModelIndex, QModelIndex)), this, SLOT(updateMoveControls()));

	copyActionsDynamicProperties();

	QItemSelectionModel *selectionModel = ui->cameraList->selectionModel();
	connect(selectionModel, SIGNAL(currentChanged(QModelIndex, QModelIndex)), this,
		SLOT(currentChanged(QModelIndex, QModelIndex)));
	connect(&accel_timer, &QTimer::timeout, this, &PTZControls::accelTimerHandler);

	connect(ui->panTiltTouch, SIGNAL(positionChanged(double, double)), this, SLOT(setPanTilt(double, double)));

	/* Right-click on the dock's Home button → "Save current position as
	 * Home" (only shown for protocols that override supportsSetHome()).
	 * Single-click still triggers GotoHome; the context menu is purely
	 * additive. */
	ui->panTiltButton_home->setContextMenuPolicy(Qt::CustomContextMenu);
	connect(ui->panTiltButton_home, &QWidget::customContextMenuRequested, this,
		&PTZControls::onHomeButtonContextMenu);

	joystickSetup();

	LoadConfig();

	/* Install an event filter to keep buttons square */
	auto filter = new squareResizeFilter(this);
	ui->movementControlsWidget->installEventFilter(filter);
	ui->pantiltStack->installEventFilter(filter);

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
		auto *button = static_cast<QToolButton *>(button_data);
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
	auto autofocustogglecb = [](void *ptz_data, obs_hotkey_id, obs_hotkey_t *, bool pressed) {
		PTZControls *ptzctrl = static_cast<PTZControls *>(ptz_data);
		if (pressed)
			ptzctrl->on_focusButton_auto_clicked(!ptzctrl->ui->focusButton_auto->isChecked());
	};
	auto autofocusoncb = [](void *ptz_data, obs_hotkey_id, obs_hotkey_t *, bool pressed) {
		PTZControls *ptzctrl = static_cast<PTZControls *>(ptz_data);
		if (pressed)
			ptzctrl->on_focusButton_auto_clicked(true);
	};
	auto autofocusoffcb = [](void *ptz_data, obs_hotkey_id, obs_hotkey_t *, bool pressed) {
		PTZControls *ptzctrl = static_cast<PTZControls *>(ptz_data);
		if (pressed)
			ptzctrl->on_focusButton_auto_clicked(false);
	};
	registerHotkey("PTZ.PanTiltUpLeft", obs_module_text("PTZ.Action.PanTiltUpLeft"), cb, ui->panTiltButton_upleft);
	registerHotkey("PTZ.PanTiltLeft", obs_module_text("PTZ.Action.PanTiltLeft"), cb, ui->panTiltButton_left);
	registerHotkey("PTZ.PanTiltDownLeft", obs_module_text("PTZ.Action.PanTiltDownLeft"), cb,
		       ui->panTiltButton_downleft);
	registerHotkey("PTZ.PanTiltUpRight", obs_module_text("PTZ.Action.PanTiltUpRight"), cb,
		       ui->panTiltButton_upright);
	registerHotkey("PTZ.PanTiltRight", obs_module_text("PTZ.Action.PanTiltRight"), cb, ui->panTiltButton_right);
	registerHotkey("PTZ.PanTiltDownRight", obs_module_text("PTZ.Action.PanTiltDownRight"), cb,
		       ui->panTiltButton_downright);
	registerHotkey("PTZ.PanTiltUp", obs_module_text("PTZ.Action.PanTiltUp"), cb, ui->panTiltButton_up);
	registerHotkey("PTZ.PanTiltDown", obs_module_text("PTZ.Action.PanTiltDown"), cb, ui->panTiltButton_down);
	registerHotkey("PTZ.ZoomWide", obs_module_text("PTZ.Action.ZoomWide"), cb, ui->zoomButton_wide);
	registerHotkey("PTZ.ZoomTele", obs_module_text("PTZ.Action.ZoomTele"), cb, ui->zoomButton_tele);
	registerHotkey("PTZ.FocusAutoToggle", obs_module_text("PTZ.Action.FocusAutoToggle"), autofocustogglecb, this);
	registerHotkey("PTZ.FocusAutoOn", obs_module_text("PTZ.Action.FocusAutoOn"), autofocusoncb, this);
	registerHotkey("PTZ.FocusAutoOff", obs_module_text("PTZ.Action.FocusAutoOff"), autofocusoffcb, this);
	registerHotkey("PTZ.FocusNear", obs_module_text("PTZ.Action.FocusNear"), cb, ui->focusButton_far);
	registerHotkey("PTZ.FocusFar", obs_module_text("PTZ.Action.FocusFar"), cb, ui->focusButton_near);
	registerHotkey("PTZ.FocusOneTouch", obs_module_text("PTZ.Action.FocusOneTouch"), cb, ui->focusButton_onetouch);
	registerHotkey("PTZ.SelectPrev", obs_module_text("PTZ.Action.SelectPrev"), prevcb, ui->cameraList);
	registerHotkey("PTZ.SelectNext", obs_module_text("PTZ.Action.SelectNext"), nextcb, ui->cameraList);
	registerHotkey(
		"PTZ.ScenePrev", obs_module_text("PTZ.Action.ScenePrev"),
		[](void *, obs_hotkey_id, obs_hotkey *, bool pressed) {
			if (pressed)
				change_current_scene(-1);
		},
		nullptr);
	registerHotkey(
		"PTZ.SceneNext", obs_module_text("PTZ.Action.SceneNext"),
		[](void *, obs_hotkey_id, obs_hotkey *, bool pressed) {
			if (pressed)
				change_current_scene(1);
		},
		nullptr);

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
		auto description = QString(obs_module_text("PTZ.Action.Preset.RecallNum")).arg(i + 1);
		auto hotkey = registerHotkey(QT_TO_UTF8(name), QT_TO_UTF8(description), preset_recall_cb, this);
		preset_hotkey_map[hotkey] = i;
		name = QString("PTZ.Save%1").arg(i + 1);
		description = QString(obs_module_text("PTZ.Action.Preset.SaveNum")).arg(i + 1);
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
	m_joystick_deadzone = std::clamp(deadzone, 0.0, 0.5);
	/* Immediatly apply the deadzone */
	auto jd = QJoysticks::getInstance()->getInputDevice(m_joystick_id);
	joystickAxesChanged(jd, 0b11111111);
}

double PTZControls::readAxis(const QJoystickDevice *jd, int axis, bool invert)
{
	if (axis < 0 || !jd || axis >= jd->axes.size())
		return 0.0;
	double v = jd->axes.at(axis) * (invert ? -1 : 1);
	if (abs(v) < m_joystick_deadzone)
		return 0.0;
	return std::copysign((abs(v) - m_joystick_deadzone) / (1.0 - m_joystick_deadzone), v);
}

void PTZControls::joystickAxesChanged(const QJoystickDevice *jd, uint32_t updated)
{
	bool isLocked = ui->cameraList->currentIndex().data(PTZListModel::IsLockedRole).toBool();
	if (isLocked || !m_joystick_enable || !jd || jd->id != m_joystick_id)
		return;
	int panTiltMask = (1 << joystick_pan_axis) | (1 << joystick_tilt_axis);
	if (updated & panTiltMask)
		setPanTilt(readAxis(jd, joystick_pan_axis, joystick_pan_invert),
			   -readAxis(jd, joystick_tilt_axis, joystick_tilt_invert));
	if (updated & (1 << joystick_zoom_axis))
		setZoom(-readAxis(jd, joystick_zoom_axis, joystick_zoom_invert));
	if (updated & (1 << joystick_focus_axis))
		setFocus(-readAxis(jd, joystick_focus_axis, joystick_focus_invert));
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

static obs_hotkey_id lookup_obs_hotkey_id(QString hotkey_name)
{
	obs_hotkey_id id = OBS_INVALID_HOTKEY_ID;
	auto data = std::make_tuple(hotkey_name, &id);
	using data_t = decltype(data);
	if (hotkey_name == "")
		return OBS_INVALID_HOTKEY_ID;
	obs_enum_hotkeys(
		[](void *data, obs_hotkey_id id, obs_hotkey_t *key) {
			data_t &d = *static_cast<data_t *>(data);
			if (get<0>(d) == obs_hotkey_get_name(key)) {
				*get<1>(d) = id;
				return false;
			}
			return true;
		},
		&data);
	return id;
}

void PTZControls::joystickButtonEvent(const QJoystickButtonEvent evt)
{
	if (!m_joystick_enable || evt.joystick->id != m_joystick_id)
		return;
	auto hotkey_id = lookup_obs_hotkey_id(joystick_button_hotkey_mappings[evt.button]);
	if (hotkey_id != OBS_INVALID_HOTKEY_ID)
		obs_hotkey_trigger_routed_callback(hotkey_id, evt.pressed);
}
#endif /* ENABLE_JOYSTICK */

void PTZControls::setJoystickAxisAction(size_t axis, ptz_joy_action_id action)
{
	if (joystick_axis_actions[axis] == action)
		return;
	joystick_axis_actions[axis] = action;

	// Clear if this is an axis already used
	if (joystick_pan_axis == (int)axis)
		joystick_pan_axis = -1;
	if (joystick_tilt_axis == (int)axis)
		joystick_tilt_axis = -1;
	if (joystick_zoom_axis == (int)axis)
		joystick_zoom_axis = -1;
	if (joystick_focus_axis == (int)axis)
		joystick_focus_axis = -1;

	int old_axis = -1;
	if ((action == PTZ_JOY_ACTION_PAN || action == PTZ_JOY_ACTION_PAN_INVERT) && joystick_pan_axis != (int)axis) {
		old_axis = joystick_pan_axis;
		joystick_pan_axis = (int)axis;
		joystick_pan_invert = action == PTZ_JOY_ACTION_PAN_INVERT;
	} else if ((action == PTZ_JOY_ACTION_TILT || action == PTZ_JOY_ACTION_TILT_INVERT) &&
		   joystick_tilt_axis != (int)axis) {
		old_axis = joystick_tilt_axis;
		joystick_tilt_axis = (int)axis;
		joystick_tilt_invert = action == PTZ_JOY_ACTION_TILT_INVERT;
	} else if ((action == PTZ_JOY_ACTION_ZOOM || action == PTZ_JOY_ACTION_ZOOM_INVERT) &&
		   joystick_zoom_axis != (int)axis) {
		old_axis = joystick_zoom_axis;
		joystick_zoom_axis = (int)axis;
		joystick_zoom_invert = action == PTZ_JOY_ACTION_ZOOM_INVERT;
	} else if ((action == PTZ_JOY_ACTION_FOCUS || action == PTZ_JOY_ACTION_FOCUS_INVERT) &&
		   joystick_focus_axis != (int)axis) {
		old_axis = joystick_focus_axis;
		joystick_focus_axis = (int)axis;
		joystick_focus_invert = action == PTZ_JOY_ACTION_FOCUS_INVERT;
	}
	if (old_axis != -1)
		emit joystickAxisActionChanged(old_axis, PTZ_JOY_ACTION_NONE);
	emit joystickAxisActionChanged(axis, action);
}

void PTZControls::setJoystickButtonHotkey(size_t button, QString hotkey_name)
{
	if (joystick_button_hotkey_mappings[button] == hotkey_name)
		return;
	if (hotkey_name == "") {
		joystick_button_hotkey_mappings.remove(button);
		emit joystickButtonHotkeyChanged(button, "");
		return;
	}
	joystick_button_hotkey_mappings[button] = hotkey_name;
	emit joystickButtonHotkeyChanged(button, hotkey_name);
}

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

	OBSDataAutoRelease savedata = obs_data_create();

	obs_data_set_string(savedata, "splitter_state", ui->splitter->saveState().toBase64().constData());
	obs_data_set_string(savedata, "vertsplitter_state", ui->vertsplitter->saveState().toBase64().constData());

	obs_data_set_bool(savedata, "live_moves_disabled", liveMovesDisabled());
	obs_data_set_bool(savedata, "autoselect_enabled", autoselectEnabled());
	obs_data_set_bool(savedata, "speed_ramp_enabled", speedRampEnabled());
	obs_data_set_bool(savedata, "onscreen_joystick_enabled", ui->pantiltStack->currentIndex() != 0);
	obs_data_set_bool(savedata, "joystick_enable", m_joystick_enable);
	obs_data_set_int(savedata, "joystick_id", m_joystick_id);
	obs_data_set_double(savedata, "joystick_speed", m_joystick_speed);
	obs_data_set_double(savedata, "joystick_deadzone", m_joystick_deadzone);

	OBSDataArrayAutoRelease axis_actions = obs_data_array_create();
	for (auto axis : joystick_axis_actions.keys()) {
		if (joystick_axis_actions[axis] == PTZ_JOY_ACTION_NONE)
			continue;
		OBSDataAutoRelease d = obs_data_create();
		obs_data_set_int(d, "axis", axis);
		obs_data_set_int(d, "action", joystick_axis_actions[axis]);
		obs_data_array_push_back(axis_actions, d);
	}
	obs_data_set_array(savedata, "joystick_axis_actions", axis_actions);

	OBSDataArrayAutoRelease button_actions = obs_data_array_create();
	for (auto button : joystick_button_hotkey_mappings.keys()) {
		auto hotkey_name = joystick_button_hotkey_mappings[button];
		if (hotkey_name == "")
			continue;
		OBSDataAutoRelease d = obs_data_create();
		obs_data_set_int(d, "button", button);
		obs_data_set_string(d, "hotkey", QT_TO_UTF8(hotkey_name));
		obs_data_array_push_back(button_actions, d);
	}
	obs_data_set_array(savedata, "joystick_button_hotkeys", button_actions);
	if (ui->cameraList->currentIndex().isValid())
		obs_data_set_int(savedata, "current_selected",
				 ui->cameraList->currentIndex().data(PTZListModel::DeviceIdRole).toInt());

	OBSDataArray camera_array = ptz_devices_get_config();
	obs_data_array_release(camera_array);
	obs_data_set_array(savedata, "devices", camera_array);

	/* Save data structure to json */
	if (!obs_data_save_json_pretty_safe(savedata, file, "tmp", "bak")) {
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

	if (!file)
		return;

	OBSDataAutoRelease loaddata = obs_data_create_from_json_file_safe(file, "bak");
	if (!loaddata) {
		/* Try loading from the old configuration path */
		auto f = QString(file).replace("obs-ptz", "ptz-controls");
		loaddata = obs_data_create_from_json_file_safe(QT_TO_UTF8(f), "bak");
	}
	bfree(file);
	if (!loaddata)
		return;
	obs_data_set_default_int(loaddata, "current_speed", 50);
	obs_data_set_default_int(loaddata, "debug_log_level", LOG_INFO);
	obs_data_set_default_bool(loaddata, "live_moves_disabled", true);
	obs_data_set_default_bool(loaddata, "autoselect_enabled", true);
	obs_data_set_default_bool(loaddata, "speed_ramp_enabled", true);
	obs_data_set_default_bool(loaddata, "onscreen_joystick_enabled", false);
	obs_data_set_default_bool(loaddata, "joystick_enable", false);
	obs_data_set_default_int(loaddata, "joystick_id", -1);
	obs_data_set_default_double(loaddata, "joystick_speed", 1.0);
	obs_data_set_default_double(loaddata, "joystick_deadzone", 0.0);

	live_moves_disabled = obs_data_get_bool(loaddata, "live_moves_disabled");
	autoselect_enabled = obs_data_get_bool(loaddata, "autoselect_enabled");
	speed_ramp_enabled = obs_data_get_bool(loaddata, "speed_ramp_enabled");
	ui->pantiltStack->setCurrentIndex(obs_data_get_bool(loaddata, "onscreen_joystick_enabled") ? 1 : 0);
	m_joystick_enable = obs_data_get_bool(loaddata, "joystick_enable");
	m_joystick_id = (int)obs_data_get_int(loaddata, "joystick_id");
	m_joystick_speed = obs_data_get_double(loaddata, "joystick_speed");
	m_joystick_deadzone = obs_data_get_double(loaddata, "joystick_deadzone");

	OBSDataArrayAutoRelease axis_actions = obs_data_get_array(loaddata, "joystick_axis_actions");
	for (size_t i = 0; i < obs_data_array_count(axis_actions); i++) {
		OBSDataAutoRelease d = obs_data_array_item(axis_actions, i);
		auto axis = obs_data_get_int(d, "axis");
		auto action = obs_data_get_int(d, "action");
		setJoystickAxisAction(axis, action);
	}

	OBSDataArrayAutoRelease button_actions = obs_data_get_array(loaddata, "joystick_button_hotkeys");
	for (size_t i = 0; i < obs_data_array_count(button_actions); i++) {
		OBSDataAutoRelease d = obs_data_array_item(button_actions, i);
		auto button = obs_data_get_int(d, "button");
		auto hotkey_name = obs_data_get_string(d, "hotkey");
		setJoystickButtonHotkey(button, hotkey_name);
	}

	const char *splitterStateStr = obs_data_get_string(loaddata, "splitter_state");
	if (splitterStateStr) {
		QByteArray splitterState = QByteArray::fromBase64(QByteArray(splitterStateStr));
		ui->splitter->restoreState(splitterState);
	}

	const char *vertsplitterStateStr = obs_data_get_string(loaddata, "vertsplitter_state");
	if (vertsplitterStateStr) {
		QByteArray splitterState = QByteArray::fromBase64(QByteArray(vertsplitterStateStr));
		ui->vertsplitter->restoreState(splitterState);
	}

	array = obs_data_get_array(loaddata, "devices");
	obs_data_array_release(array);
	ptz_devices_set_config(array);
	ui->cameraList->setCurrentIndex(
		ptzDeviceList.indexFromDeviceId(obs_data_get_int(loaddata, "current_selected")));
}

void PTZControls::setAutoselectEnabled(bool enabled)
{
	if (enabled == autoselect_enabled)
		return;
	autoselect_enabled = enabled;
	emit autoselectEnabledChanged(enabled);
}

void PTZControls::setDisableLiveMoves(bool disable)
{
	if (disable == live_moves_disabled)
		return;
	live_moves_disabled = disable;
	updateMoveControls();
	emit liveMovesDisabledChanged(disable);
}

void PTZControls::setSpeedRampEnabled(bool enabled)
{
	if (enabled == speed_ramp_enabled)
		return;
	speed_ramp_enabled = enabled;
	emit speedRampEnabledChanged(enabled);
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
	double speed = 0.5;
	double ramp = 0;

	if (modifiers.testFlag(Qt::ControlModifier))
		speed = 1.0;
	else if (modifiers.testFlag(Qt::ShiftModifier))
		speed = 0.05;
	else if (speedRampEnabled())
		speed = ramp = 0.05;

	setPanTilt(pan * speed, tilt * speed, pan * ramp, tilt * ramp);
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

	auto modifiers = QGuiApplication::keyboardModifiers();
	double speed = 0.5;
	zoomingFlag = (zoom != 0.0);
	if (modifiers.testFlag(Qt::ControlModifier))
		speed = 1.0;
	else if (modifiers.testFlag(Qt::ShiftModifier))
		speed = 0.1;

	ptz->zoom(zoom * speed);
}

void PTZControls::setFocus(double focus)
{
	PTZDevice *ptz = currCamera();
	if (!ptz)
		return;

	auto modifiers = QGuiApplication::keyboardModifiers();
	double speed = 0.5;
	focusingFlag = (focus != 0.0);
	if (modifiers.testFlag(Qt::ControlModifier))
		speed = 1.0;
	else if (modifiers.testFlag(Qt::ShiftModifier))
		speed = 0.1;

	ptz->focus(focus * speed);
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

void PTZControls::onHomeButtonContextMenu(const QPoint &pos)
{
	if (!ui->cameraList->currentIndex().data(PTZListModel::SupportsSetHomeRole).toBool())
		return;
	QMenu menu(this);
	QAction *setHome = menu.addAction(obs_module_text("PTZ.Action.SetHome"));
	QAction *picked = menu.exec(ui->panTiltButton_home->mapToGlobal(pos));
	if (picked == setHome) {
		auto ptz = currCamera();
		if (ptz)
			ptz->pantilt_set_home();
	}
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
	ui->cameraList->setCurrentIndex(ptzDeviceList.indexFromDeviceId(device_id));
}

void PTZControls::setAutofocusEnabled(bool autofocus_on)
{
	ui->focusButton_auto->setChecked(autofocus_on);
	ui->focusButton_near->setEnabled(!autofocus_on);
	ui->focusButton_far->setEnabled(!autofocus_on);
	ui->focusButton_onetouch->setEnabled(!autofocus_on);
}

void PTZControls::updateMoveControls()
{
	bool is_locked = ui->cameraList->currentIndex().data(PTZListModel::IsLockedRole).toBool();

	ui->movementControlsWidget->setEnabled(!is_locked);
	ui->presetListView->setEnabled(!is_locked);

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

		settingsChanged(ptz->get_settings());
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
	if (index.isValid())
		return index.data(Qt::UserRole).toInt();
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

	QAction *touchpadAction = menu.addAction(obs_module_text("PTZ.Dock.OnscreenJoystick"));
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
	QMenu presetContext;
	if (index.isValid()) {
		presetContext.addAction(ui->actionPresetRename);
		presetContext.addAction(ui->actionPresetSave);
		presetContext.addAction(ui->actionPresetClear);
		presetContext.addAction(ui->actionPresetRemove);
	}
	presetContext.addAction(ui->actionPresetAdd);
	presetContext.exec(globalpos);
}

void PTZControls::on_cameraList_doubleClicked(const QModelIndex &index)
{
	ptz_settings_show(index);
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
		powerAction =
			context.addAction(obs_module_text(power_on ? "PTZ.Action.PowerOff" : "PTZ.Action.PowerOn"));

		bool wb_onepush = (obs_data_get_int(settings, "wb_mode") == 3);
		if (wb_onepush)
			wbOnetouchAction = context.addAction(obs_module_text("PTZ.Action.WhiteBalance.OnePushTrigger"));
		context.addSeparator();
	}
	QAction *autoselectAction = context.addAction(obs_module_text("PTZ.Settings.CameraAutoselect"));
	autoselectAction->setCheckable(true);
	autoselectAction->setChecked(autoselectEnabled());
	connect(autoselectAction, SIGNAL(toggled(bool)), this, SLOT(setAutoselectEnabled(bool)));
	if (obs_frontend_preview_program_mode_active()) {
		QAction *blockliveAction = context.addAction(obs_module_text("PTZ.Settings.BlockLiveMoves"));
		blockliveAction->setCheckable(true);
		blockliveAction->setChecked(liveMovesDisabled());
		connect(blockliveAction, SIGNAL(toggled(bool)), this, SLOT(setDisableLiveMoves(bool)));
	}
	context.addAction(ui->actionProperties);
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
	}
}

void PTZControls::on_actionProperties_triggered()
{
	ptz_settings_show(ui->cameraList->currentIndex());
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

void PTZControls::on_actionPresetRename_triggered()
{
	auto model = ui->presetListView->model();
	auto index = ui->presetListView->currentIndex();
	if (!model || !index.isValid())
		return;
	ui->presetListView->edit(index);
}

void PTZControls::on_actionPresetSave_triggered()
{
	auto model = ui->presetListView->model();
	auto index = ui->presetListView->currentIndex();
	PTZDevice *ptz = currCamera();
	if (!model || !index.isValid() || !ptz)
		return;
	ptz->memory_set(presetIndexToId(index));
}

void PTZControls::on_actionPresetClear_triggered()
{
	auto model = ui->presetListView->model();
	auto index = ui->presetListView->currentIndex();
	PTZDevice *ptz = currCamera();
	if (!model || !index.isValid() || !ptz)
		return;
	ptz->memory_reset(presetIndexToId(index));
	ui->presetListView->model()->setData(index, "");
}

PTZDeviceListDelegate::PTZDeviceListDelegate(QObject *parent) : QStyledItemDelegate(parent) {}

QSize PTZDeviceListDelegate::sizeHint(const QStyleOptionViewItem &option, const QModelIndex &index) const
{
	return QStyledItemDelegate::sizeHint(option, index).expandedTo(QSize(40, 0));
}

void PTZDeviceListDelegate::paint(QPainter *painter, const QStyleOptionViewItem &option, const QModelIndex &index) const
{
	auto newopt = option;

	bool isLive = index.data(PTZListModel::IsLiveRole).toBool();
	bool isLocked = index.data(PTZListModel::IsLockedRole).toBool();
	bool isConnected = index.data(PTZListModel::IsConnectedRole).toBool();
	bool isDark = obs_frontend_is_theme_dark();

	if (isLive) {
		QIcon locked(isDark ? "theme:Dark/locked.svg" : ":res/images/locked.svg");
		QIcon unlocked(":res/images/unlocked.svg");
		QRect r(newopt.rect.right() - 18, newopt.rect.top() + 2, 16, 16);
		newopt.rect = newopt.rect.marginsRemoved(QMargins(0, 0, 20, 0));
		if (isLocked)
			locked.paint(painter, r);
		else
			unlocked.paint(painter, r);
	}

	if (!isConnected) {
		QIcon disconnected(isDark ? "theme:Dark/no_sources.svg" : ":res/images/no_sources.svg");
		QRect r(newopt.rect.right() - 18, newopt.rect.top() + 2, 16, 16);
		newopt.rect = newopt.rect.marginsRemoved(QMargins(0, 0, 20, 0));
		disconnected.paint(painter, r);
	}

	QStyledItemDelegate::paint(painter, newopt, index);
}

bool PTZDeviceListDelegate::editorEvent(QEvent *event, QAbstractItemModel *model, const QStyleOptionViewItem &option,
					const QModelIndex &index)
{
	bool isLive = index.data(PTZListModel::IsLiveRole).toBool();
	if (event && event->type() == QEvent::MouseButtonRelease && isLive) {
		bool isLocked = index.data(PTZListModel::IsLockedRole).toBool();
		auto pos = static_cast<QMouseEvent *>(event)->pos() - option.rect.topRight();
		if (pos.x() > -20)
			model->setData(index, !isLocked, PTZListModel::IsLockedRole);
	}
	return QStyledItemDelegate::editorEvent(event, model, option, index);
}
