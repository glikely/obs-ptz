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
#include <QLabel>
#include <QCheckBox>
#include <QFileDialog>
#include <QMessageBox>

#include <qt-wrappers.hpp>
#include "touch-control.hpp"
#include "ui_ptz-controls.h"
#include "ptz-controls.hpp"
#include "ptz-list-model.hpp"
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

void PTZControls::autoselectDevice(OBSSource scene)
{
	auto active_src_cb = [](obs_source_t *, obs_source_t *child, void *data) {
		auto index = static_cast<QModelIndex *>(data);
		if (!index->isValid())
			*index = ptzDeviceList.indexFromName(obs_source_get_name(child));
	};
	QModelIndex index = ptzDeviceList.indexFromName(obs_source_get_name(scene));
	if (!index.isValid())
		obs_source_enum_active_sources(scene, active_src_cb, &index);

	if (index.isValid())
		ui->deviceList->setCurrentIndex(index);
}

void PTZControls::onFrontendEvent(enum obs_frontend_event event, void *ptr)
{
	PTZControls *controls = reinterpret_cast<PTZControls *>(ptr);
	controls->handleFrontendEvent(event);
}

void PTZControls::onFrontendSaveEvent(obs_data_t *save_data, bool saving, void *ptr)
{
	/* This plugin's configuration lives in its own file rather than the
	 * scene collection's save_data, so only the "saving" direction is of
	 * interest here; loading still happens once at startup in LoadConfig() */
	Q_UNUSED(save_data);
	if (saving)
		reinterpret_cast<PTZControls *>(ptr)->SaveConfig();
}

void PTZControls::handleFrontendEvent(enum obs_frontend_event event)
{
	switch (event) {
	case OBS_FRONTEND_EVENT_TRANSITION_STOPPED:
		updateMoveControls();
		break;
	case OBS_FRONTEND_EVENT_SCENE_CHANGED:
		if (autoselectEnabled() && !obs_frontend_preview_program_mode_active()) {
			OBSSourceAutoRelease source = obs_frontend_get_current_scene();
			autoselectDevice(source.Get());
		}
		ptzDeviceList.onSceneChanged();
		updateMoveControls();
		break;
	case OBS_FRONTEND_EVENT_STUDIO_MODE_ENABLED:
		if (autoselectEnabled()) {
			OBSSourceAutoRelease source = obs_frontend_get_current_scene();
			autoselectDevice(source.Get());
		}
		ptzDeviceList.onSceneChanged();
		updateMoveControls();
		break;
	case OBS_FRONTEND_EVENT_STUDIO_MODE_DISABLED:
	case OBS_FRONTEND_EVENT_PREVIEW_SCENE_CHANGED:
		if (autoselectEnabled() && obs_frontend_preview_program_mode_active()) {
			OBSSourceAutoRelease source = obs_frontend_get_current_preview_scene();
			autoselectDevice(source.Get());
		}
		ptzDeviceList.onSceneChanged();
		updateMoveControls();
		break;
	case OBS_FRONTEND_EVENT_EXIT:
		/* OBS is shutting down. It has already run its own save pass (and
		 * so has called onFrontendSaveEvent()) as part of its shutdown
		 * sequence, so just remove the PTZDevice instances here */
		while (!hotkeys.isEmpty())
			obs_hotkey_unregister(hotkeys.takeFirst());
		obs_frontend_remove_event_callback(onFrontendEvent, this);
		obs_frontend_remove_save_callback(onFrontendSaveEvent, this);
		ptzDeviceList.delete_all();
		break;
	case OBS_FRONTEND_EVENT_THEME_CHANGED:
		/* Defer call with a singleShot to let all layout changes settle */
		QTimer::singleShot(0, this, &PTZControls::refreshTheme);
		break;
	default:
		break;
	}
}

/* The theme has changed; recalculate the icon and list row heights to
 * match the stock OBS theme */
void PTZControls::refreshTheme()
{
	int densityId = -4;
#if LIBOBS_API_VER >= MAKE_SEMANTIC_VERSION(31, 0, 0)
	if (config_t *cfg = obs_frontend_get_user_config())
#else
	/* Fallback to deprecated API when building against older OBS */
	if (config_t *cfg = obs_frontend_get_global_config())
#endif
		densityId = (int)config_get_int(cfg, "Appearance", "Density");

	int rowHeightFloor, fontHeightOffset;
	switch (densityId) {
	case -2: /* Classic */
		rowHeightFloor = 18;
		fontHeightOffset = 2;
		break;
	case -3: /* Compact */
		rowHeightFloor = 22;
		fontHeightOffset = 4;
		break;
	case -5: /* Comfortable */
		rowHeightFloor = 36;
		fontHeightOffset = 10;
		break;
	case -4: /* Normal (default) */
	default:
		rowHeightFloor = 30;
		fontHeightOffset = 8;
		break;
	}

	QLabel fontProbe;
	fontProbe.setFont(font());
	fontProbe.setText(QStringLiteral("Ag"));
	int fontHeight = fontProbe.sizeHint().height();
	m_rowHeight = qMax(rowHeightFloor, fontHeight + fontHeightOffset);

	QCheckBox iconProbe;
	iconProbe.setProperty("class", "checkbox-icon");
	m_iconSize = iconProbe.style()->pixelMetric(QStyle::PM_IndicatorHeight, nullptr, &iconProbe);

	if (presetDelegate)
		presetDelegate->refreshTheme();
	if (deviceDelegate)
		deviceDelegate->refreshTheme();
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

	/* The directional pan/tilt/zoom/focus buttons each have their own
	 * translated tooltip describing the action, but all share the same
	 * "held modifier key" hint. Append it here instead of repeating it
	 * (and its markup) in every individual translation string. */
	const QString modifierHint = QString("%1\n%2")
					     .arg(obs_module_text("PTZ.Action.Movement.Tooltip.Fast"))
					     .arg(obs_module_text("PTZ.Action.Movement.Tooltip.Slow"));
	for (QWidget *w :
	     {ui->panTiltButton_upleft, ui->panTiltButton_up, ui->panTiltButton_upright, ui->panTiltButton_left,
	      ui->panTiltButton_right, ui->panTiltButton_downleft, ui->panTiltButton_down, ui->panTiltButton_downright,
	      ui->zoomButton_wide, ui->zoomButton_tele, ui->focusButton_near, ui->focusButton_far})
		w->setToolTip(w->toolTip() + "\n" + modifierHint);

	/* Compatability: Before OBS Studio 31.1.0 the theme had left and right
	 * margins on widgets which mess with the grid layout used by this
	 * plugin. If the version is earlier than 31.1.0 then apply an extra
	 * style sheet to fix */
	if (obs_get_version() < MAKE_SEMANTIC_VERSION(31, 1, 0))
		this->setStyleSheet("margin-left: 0px; margin-right: 0px");

	refreshTheme();

	ui->deviceList->setModel(&ptzDeviceList);
	deviceDelegate = new PTZDeviceListDelegate(ui->deviceList);
	ui->deviceList->setItemDelegate(deviceDelegate);
	connect(&ptzDeviceList, &PTZListModel::dataChanged, this, &PTZControls::settingsChanged);

	copyActionsDynamicProperties();

	QItemSelectionModel *selectionModel = ui->deviceList->selectionModel();
	connect(selectionModel, &QItemSelectionModel::currentChanged, this, &PTZControls::currentChanged);
	connect(&accel_timer, &QTimer::timeout, this, &PTZControls::accelTimerHandler);

	ui->presetListView->setModel(&ptzDeviceList);
	presetDelegate = new PTZPresetListDelegate(ui->presetListView);
	ui->presetListView->setItemDelegate(presetDelegate);
	ui->presetListView->setRootIndex(ptzDeviceList.index(0, 0));
	selectionModel = ui->presetListView->selectionModel();
	connect(selectionModel, &QItemSelectionModel::currentChanged, this, &PTZControls::presetUpdateActions);

	connect(ui->panTiltTouch, &TouchControl::positionChanged, [this](double p, double t) { setPanTilt(p, t); });

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

	obs_frontend_add_event_callback(onFrontendEvent, this);
	obs_frontend_add_save_callback(onFrontendSaveEvent, this);

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
	registerHotkey("PTZ.SelectPrev", obs_module_text("PTZ.Action.SelectPrev"), prevcb, ui->deviceList);
	registerHotkey("PTZ.SelectNext", obs_module_text("PTZ.Action.SelectNext"), nextcb, ui->deviceList);
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

	registerHotkey(
		"PTZ.PresetPrev", obs_module_text("PTZ.Action.Preset.Prev"),
		[](void *ptz_data, obs_hotkey_id, obs_hotkey_t *, bool pressed) {
			if (pressed)
				static_cast<PTZControls *>(ptz_data)->ui->presetListView->cursorUp();
		},
		this);
	registerHotkey(
		"PTZ.PresetNext", obs_module_text("PTZ.Action.Preset.Next"),
		[](void *ptz_data, obs_hotkey_id, obs_hotkey_t *, bool pressed) {
			if (pressed)
				static_cast<PTZControls *>(ptz_data)->ui->presetListView->cursorDown();
		},
		this);
	registerHotkey(
		"PTZ.PresetRecallSelected", obs_module_text("PTZ.Action.Preset.RecallSelected"),
		[](void *ptz_data, obs_hotkey_id, obs_hotkey_t *, bool pressed) {
			if (ptz_data && pressed) {
				auto ctrls = static_cast<PTZControls *>(ptz_data);
				auto index = ctrls->ui->presetListView->currentIndex();
				if (index.isValid())
					ctrls->on_presetListView_activated(index);
			}
		},
		this);
}

#if defined(ENABLE_JOYSTICK)
void PTZControls::joystickSetup()
{
	auto joysticks = QJoysticks::getInstance();
	joysticks->setVirtualJoystickEnabled(false);
	joysticks->updateInterfaces();
	connect(joysticks, &QJoysticks::axisEvent, this, &PTZControls::joystickAxisEvent);
	connect(joysticks, &QJoysticks::buttonEvent, this, &PTZControls::joystickButtonEvent);
	connect(joysticks, &QJoysticks::POVEvent, this, &PTZControls::joystickPOVEvent);
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
	bool isLocked = liveMoveLockActive() &&
			ui->deviceList->currentIndex().data(PTZListModel::IsLockedRole).toBool();
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
			if (std::get<0>(d) == obs_hotkey_get_name(key)) {
				*std::get<1>(d) = id;
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
	if (old_axis != -1) {
		/* The UI was cleared, but leaving the old action in this map caused it
		 * to be serialized and restored on the next launch. */
		joystick_axis_actions.remove(old_axis);
		emit joystickAxisActionChanged(old_axis, PTZ_JOY_ACTION_NONE);
	}
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
	for (QToolBar *toolbar : {ui->ptzToolbar, ui->presetToolbar}) {
		for (QAction *x : toolbar->actions()) {
			QWidget *temp = toolbar->widgetForAction(x);

			for (QByteArray &y : x->dynamicPropertyNames()) {
				temp->setProperty(y, x->property(y));
			}
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

	obs_data_set_bool(savedata, "live_moves_disabled", liveMoveLockEnabled());
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
	if (ui->deviceList->currentIndex().isValid())
		obs_data_set_int(savedata, "current_selected",
				 ui->deviceList->currentIndex().data(PTZListModel::DeviceIdRole).toInt());

	OBSDataArrayAutoRelease devices = obs_data_array_create();
	ptzDeviceList.save(devices.Get());
	obs_data_set_array(savedata, "devices", devices);

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

	live_move_lock_enabled = obs_data_get_bool(loaddata, "live_moves_disabled");
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
	ui->deviceList->setCurrentIndex(
		ptzDeviceList.indexFromDeviceId(obs_data_get_int(loaddata, "current_selected")));
}

void PTZControls::setAutoselectEnabled(bool enabled)
{
	if (enabled == autoselect_enabled)
		return;
	autoselect_enabled = enabled;
	emit autoselectEnabledChanged(enabled);
}

void PTZControls::setLiveMoveLockEnabled(bool enable)
{
	if (enable == live_move_lock_enabled)
		return;
	live_move_lock_enabled = enable;
	updateMoveControls();
	emit liveMoveLockEnabledChanged(enable);
}

void PTZControls::setSpeedRampEnabled(bool enabled)
{
	if (enabled == speed_ramp_enabled)
		return;
	speed_ramp_enabled = enabled;
	emit speedRampEnabledChanged(enabled);
}

/**
 * PTZControls::callCurrentDevice - Helpers for sending device commands
 *
 * These are helper methods for sending commands through to a PTZ
 * device. The first method accepts a calldata structure with all the
 * command's arguments. The variants are argument helper versions that
 * handle the most common calling cases, freeing the caller from
 * creating a calldata structure.
 *
 * The argument helpers all use a fixed-stack instance of calldata. This
 * is faster because it puts all the data onto the stack and doesn't
 * require calls to bzalloc()/bfree() in the calldata code.
 */
bool PTZControls::callCurrentDevice(const char *method, calldata_t *cd) const
{
	return ptzDeviceList.callDevice(ui->deviceList->currentIndex(), method, cd);
}

bool PTZControls::callCurrentDevice(const char *method, const char *arg, long long val) const
{
	calldata cd;
	uint8_t stack[128];
	calldata_init_fixed(&cd, stack, sizeof(stack));
	calldata_set_int(&cd, arg, val);
	return callCurrentDevice(method, &cd);
}

bool PTZControls::callCurrentDevice(const char *method, const char *arg, double val) const
{
	calldata cd;
	uint8_t stack[128];
	calldata_init_fixed(&cd, stack, sizeof(stack));
	calldata_set_float(&cd, arg, val);
	return callCurrentDevice(method, &cd);
}

bool PTZControls::callCurrentDevice(const char *method, const char *arg, bool val) const
{
	calldata cd;
	uint8_t stack[128];
	calldata_init_fixed(&cd, stack, sizeof(stack));
	calldata_set_bool(&cd, arg, val);
	return callCurrentDevice(method, &cd);
}

void PTZControls::accelTimerHandler()
{
	calldata cd;
	uint8_t stack[128];
	calldata_init_fixed(&cd, stack, sizeof(stack));

	if (!ui->deviceList->currentIndex().isValid()) {
		accel_timer.stop();
		return;
	}

	if (pan_accel || tilt_accel) {
		pan_speed = std::clamp(pan_speed + pan_accel, -1.0, 1.0);
		if (std::abs(pan_speed) == 1.0)
			pan_accel = 0.0;
		tilt_speed = std::clamp(tilt_speed + tilt_accel, -1.0, 1.0);
		if (std::abs(tilt_speed) == 1.0)
			tilt_accel = 0.0;
		calldata_set_float(&cd, "pan", pan_speed);
		calldata_set_float(&cd, "tilt", tilt_speed);
	}

	if (zoom_accel) {
		zoom_speed = std::clamp(zoom_speed + zoom_accel, -1.0, 1.0);
		if (std::abs(zoom_speed) == 1.0)
			zoom_accel = 0.0;
		calldata_set_float(&cd, "zoom", zoom_speed);
	}

	if (focus_accel) {
		focus_speed = std::clamp(focus_speed + focus_accel, -1.0, 1.0);
		if (std::abs(focus_speed) == 1.0)
			focus_accel = 0.0;
		calldata_set_float(&cd, "focus", focus_speed);
	}

	callCurrentDevice("ptz_move", &cd);

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

	if (pan_accel != 0 || tilt_accel != 0)
		accel_timer.start(2000 / 20);

	calldata cd;
	uint8_t stack[128];
	calldata_init_fixed(&cd, stack, sizeof(stack));
	calldata_set_float(&cd, "pan", pan_speed);
	calldata_set_float(&cd, "tilt", tilt_speed);
	callCurrentDevice("ptz_move", &cd);
	calldata_free(&cd);
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
	auto modifiers = QGuiApplication::keyboardModifiers();
	double speed = 0.5;
	zoomingFlag = (zoom != 0.0);
	if (modifiers.testFlag(Qt::ControlModifier))
		speed = 1.0;
	else if (modifiers.testFlag(Qt::ShiftModifier))
		speed = 0.1;

	callCurrentDevice("ptz_move", "zoom", zoom * speed);
}

void PTZControls::setFocus(double focus)
{
	auto modifiers = QGuiApplication::keyboardModifiers();
	double speed = 0.5;
	focusingFlag = (focus != 0.0);
	if (modifiers.testFlag(Qt::ControlModifier))
		speed = 1.0;
	else if (modifiers.testFlag(Qt::ShiftModifier))
		speed = 0.1;

	callCurrentDevice("ptz_move", "focus", focus * speed);
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
	callCurrentDevice("ptz_home_recall");
}

void PTZControls::onHomeButtonContextMenu(const QPoint &pos)
{
	if (!ui->deviceList->currentIndex().data(PTZListModel::SupportsSetHomeRole).toBool())
		return;
	QMenu menu(this);
	QAction *setHome = menu.addAction(obs_module_text("PTZ.Action.SetHome"));
	QAction *picked = menu.exec(ui->panTiltButton_home->mapToGlobal(pos));
	if (picked == setHome)
		callCurrentDevice("ptz_home_save");
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
	callCurrentDevice("ptz_set", "focus_af_enabled", checked);
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
	callCurrentDevice("ptz_set", "focus_onetouch_trigger", true);
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
	bool is_locked = liveMoveLockActive() &&
			 ui->deviceList->currentIndex().data(PTZListModel::IsLockedRole).toBool();

	ui->movementControlsWidget->setEnabled(!is_locked);
	ui->deviceList->update();
	ui->presetListView->setEnabled(!is_locked);

	RefreshToolBarStyling(ui->ptzToolbar);

	calldata cd = {};
	calldata_set_string(&cd, "property", "focus_af_enabled");
	callCurrentDevice("ptz_get", &cd);
	setAutofocusEnabled(calldata_bool(&cd, "focus_af_enabled"));
	calldata_free(&cd);
}

void PTZControls::currentChanged(QModelIndex current, QModelIndex previous)
{
	accel_timer.stop();
	if (pantiltingFlag || zoomingFlag || focusingFlag)
		ptzDeviceList.callDevice(previous, "ptz_stop");
	pantiltingFlag = false;
	zoomingFlag = false;
	focusingFlag = false;
	pan_speed = pan_accel = 0.0;
	tilt_speed = tilt_accel = 0.0;
	zoom_speed = zoom_accel = 0.0;
	focus_speed = focus_accel = 0.0;

	ui->presetListView->setRootIndex(current);
	updateMoveControls();
}

void PTZControls::settingsChanged(const QModelIndex &topLeft, const QModelIndex &bottomRight)
{
	auto index = ui->deviceList->currentIndex();
	QItemSelectionRange range(topLeft, bottomRight);
	if (range.contains(index))
		updateMoveControls();
}

void PTZControls::presetSet(long long preset_id)
{
	callCurrentDevice("ptz_preset_save", "preset_id", preset_id);
}

void PTZControls::presetRecall(long long preset_id)
{
	callCurrentDevice("ptz_preset_recall", "preset_id", preset_id);
}

void PTZControls::presetReset(long long preset_id)
{
	callCurrentDevice("ptz_preset_clear", "preset_id", preset_id);
}

int PTZControls::presetIndexToId(QModelIndex index)
{
	if (index.isValid())
		return index.data(Qt::UserRole).toInt();
	return -1;
}

void PTZControls::presetUpdateActions()
{
	auto presetIndex = ui->presetListView->currentIndex();
	auto deviceIndex = ui->deviceList->currentIndex();
	int count = ptzDeviceList.rowCount(deviceIndex);
	bool isValid = presetIndex.isValid() && deviceIndex.isValid();
	ui->actionPresetAdd->setEnabled(deviceIndex.isValid());
	ui->actionPresetRemove->setEnabled(isValid);
	ui->actionPresetMoveUp->setEnabled(isValid && count > 1 && presetIndex.row() > 0);
	ui->actionPresetMoveDown->setEnabled(isValid && count > 1 && presetIndex.row() < count - 1);
	ui->actionPresetExport->setEnabled(deviceIndex.isValid() && count > 0);
	ui->actionPresetImport->setEnabled(deviceIndex.isValid());
	RefreshToolBarStyling(ui->presetToolbar);
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
	presetContext.addSeparator();
	presetContext.addAction(ui->actionPresetExport);
	presetContext.addAction(ui->actionPresetImport);
	presetContext.exec(globalpos);
}

void PTZControls::on_deviceList_customContextMenuRequested(const QPoint &pos)
{
	QPoint globalpos = ui->deviceList->mapToGlobal(pos);
	QModelIndex index = ui->deviceList->indexAt(pos);
	QMenu context;
	QAction *powerAction = nullptr;
	QAction *wbOnetouchAction = nullptr;
	bool power_on = false;

	if (index.isValid()) {
		calldata cd = {};
		calldata_set_string(&cd, "property", "power_on");
		ptzDeviceList.callDevice(index, "ptz_get", &cd);
		power_on = calldata_bool(&cd, "power_on");
		powerAction =
			context.addAction(obs_module_text(power_on ? "PTZ.Action.PowerOff" : "PTZ.Action.PowerOn"));

		calldata_set_string(&cd, "property", "wb_mode");
		ptzDeviceList.callDevice(index, "ptz_get", &cd);
		bool wb_onepush = (calldata_int(&cd, "wb_mode") == 3);
		if (wb_onepush)
			wbOnetouchAction = context.addAction(obs_module_text("PTZ.Action.WhiteBalance.OnePushTrigger"));
		context.addSeparator();

		calldata_free(&cd);
	}
	QAction *autoselectAction = context.addAction(obs_module_text("PTZ.Settings.CameraAutoselect"));
	autoselectAction->setCheckable(true);
	autoselectAction->setChecked(autoselectEnabled());
	connect(autoselectAction, &QAction::toggled, this, &PTZControls::setAutoselectEnabled);
	if (obs_frontend_preview_program_mode_active()) {
		QAction *blockliveAction = context.addAction(obs_module_text("PTZ.Settings.BlockLiveMoves"));
		blockliveAction->setCheckable(true);
		blockliveAction->setChecked(liveMoveLockEnabled());
		connect(blockliveAction, &QAction::toggled, this, &PTZControls::setLiveMoveLockEnabled);
	}
	context.addAction(ui->actionProperties);
	QAction *action = context.exec(globalpos);

	if (action == nullptr)
		return;
	if (action == powerAction) {
		calldata cd = {};
		calldata_set_bool(&cd, "power_on", !power_on);
		ptzDeviceList.callDevice(index, "ptz_set", &cd);
		calldata_free(&cd);
	} else if (action == wbOnetouchAction) {
		calldata cd = {};
		calldata_set_bool(&cd, "wb_onepush_trigger", true);
		ptzDeviceList.callDevice(index, "ptz_set", &cd);
		calldata_free(&cd);
	}
}

void PTZControls::on_actionProperties_triggered()
{
	ptz_settings_show(ui->deviceList->currentIndex());
}

void PTZControls::on_actionPresetAdd_triggered()
{
	auto parent = ui->deviceList->currentIndex();
	auto row = ptzDeviceList.rowCount(parent);
	ptzDeviceList.insertRows(row, 1, parent);
	QModelIndex index = ptzDeviceList.index(row, 0, parent);
	if (index.isValid()) {
		ui->presetListView->setCurrentIndex(index);
		ui->presetListView->edit(index);
	}
	presetUpdateActions();
}

void PTZControls::on_actionPresetRemove_triggered()
{
	auto index = ui->presetListView->currentIndex();
	if (!index.isValid())
		return;
	ptzDeviceList.removeRows(index.row(), 1, ui->deviceList->currentIndex());
	presetUpdateActions();
}

void PTZControls::on_actionPresetMoveUp_triggered()
{
	auto index = ui->presetListView->currentIndex();
	auto parent = ui->deviceList->currentIndex();
	if (!index.isValid())
		return;
	ptzDeviceList.moveRow(parent, index.row(), parent, index.row() - 1);
	presetUpdateActions();
}

void PTZControls::on_actionPresetMoveDown_triggered()
{
	auto index = ui->presetListView->currentIndex();
	auto parent = ui->deviceList->currentIndex();
	if (!index.isValid())
		return;
	ptzDeviceList.moveRow(parent, index.row(), parent, index.row() + 2);
	presetUpdateActions();
}

void PTZControls::on_actionPresetRename_triggered()
{
	auto index = ui->presetListView->currentIndex();
	if (!index.isValid())
		return;
	ui->presetListView->edit(index);
}

void PTZControls::on_actionPresetSave_triggered()
{
	auto index = ui->presetListView->currentIndex();
	if (!index.isValid())
		return;
	presetSet(presetIndexToId(index));
}

void PTZControls::on_actionPresetClear_triggered()
{
	auto index = ui->presetListView->currentIndex();
	if (!index.isValid())
		return;
	presetReset(presetIndexToId(index));
	ptzDeviceList.setData(index, "");
}

void PTZControls::on_actionPresetExport_triggered(QString filename)
{
	auto fileExtension = QString("%1 (*.json)").arg(obs_module_text("PTZ.Preset.FileFilter"));
	QModelIndex index = ui->deviceList->currentIndex();
	if (!index.isValid())
		return;

	QString deviceName = ptzDeviceList.data(index, Qt::DisplayRole).toString();
	QString defaultName = QString(deviceName).replace(QLatin1Char('/'), QLatin1Char('_')) + " presets.json";
	if (filename.isEmpty())
		filename = QFileDialog::getSaveFileName(this, obs_module_text("PTZ.Action.Preset.Export"), defaultName,
							fileExtension);
	if (filename.isEmpty())
		return;

	/* save() serializes the device's whole config; presets/preset_max are
	 * just the subset of that we actually want in the exported file. */
	OBSDataAutoRelease fullConfig = obs_data_create();
	ptzDeviceList.save(index, fullConfig.Get());

	OBSDataAutoRelease data = obs_data_create();
	obs_data_set_int(data, "obs-ptz-preset-format", 1);
	obs_data_set_string(data, "device", QT_TO_UTF8(deviceName));
	obs_data_set_int(data, "preset_max", obs_data_get_int(fullConfig, "preset_max"));
	OBSDataArrayAutoRelease presets = obs_data_get_array(fullConfig, "presets");
	obs_data_set_array(data, "presets", presets);

	if (!obs_data_save_json_pretty_safe(data, QT_TO_UTF8(filename), "tmp", "bak"))
		QMessageBox::warning(this, obs_module_text("PTZ.Action.Preset.Export"),
				     obs_module_text("PTZ.Preset.Export.Failed"));
}

void PTZControls::on_actionPresetImport_triggered(QString filename)
{
	auto fileExtension = QString("%1 (*.json)").arg(obs_module_text("PTZ.Preset.FileFilter"));
	QModelIndex index = ui->deviceList->currentIndex();
	if (!index.isValid())
		return;

	if (filename.isEmpty())
		filename = QFileDialog::getOpenFileName(this, obs_module_text("PTZ.Action.Preset.Import"), QString(),
							fileExtension);
	if (filename.isEmpty())
		return;

	OBSDataAutoRelease data = obs_data_create_from_json_file(QT_TO_UTF8(filename));
	if (!data || obs_data_get_int(data, "obs-ptz-preset-format") != 1) {
		QMessageBox::warning(this, obs_module_text("PTZ.Action.Preset.Import"),
				     obs_module_text("PTZ.Preset.Import.Failed"));
		return;
	}

	/* Save current selected device */
	uint32_t deviceId = ptzDeviceList.data(index, PTZListModel::DeviceIdRole).toUInt();

	/* Merge just the presets/preset_max subset from the imported file
	 * into the device's current full config, then update() with that --
	 * update()'s own defaulting would otherwise reset every other
	 * setting (pan/tilt speed, invert flags, ...) to its default, since
	 * this file only ever has the two preset-related keys. */
	OBSDataAutoRelease fullConfig = obs_data_create();
	ptzDeviceList.save(index, fullConfig.Get());
	if (obs_data_has_user_value(data, "preset_max"))
		obs_data_set_int(fullConfig, "preset_max", obs_data_get_int(data, "preset_max"));
	OBSDataArrayAutoRelease presets = obs_data_get_array(data, "presets");
	obs_data_set_array(fullConfig, "presets", presets);

	ptzDeviceList.update(index, fullConfig.Get());
	ptzDeviceList.do_reset();
	/* restore selection after reset */
	ui->deviceList->setCurrentIndex(ptzDeviceList.indexFromDeviceId(deviceId));
	presetUpdateActions();
}

PTZDeviceListDelegate::PTZDeviceListDelegate(QObject *parent) : QStyledItemDelegate(parent)
{
	refreshTheme();
}

void PTZDeviceListDelegate::refreshTheme()
{
	bool isDark = obs_frontend_is_theme_dark();
	lockedIcon = QIcon(isDark ? "theme:Dark/locked.svg" : ":res/images/locked.svg");
	unlockedIcon = QIcon(":res/images/unlocked.svg");
	disconnectedIcon = QIcon(isDark ? "theme:Dark/alert.svg" : ":res/images/alert.svg");

	emit sizeHintChanged(QModelIndex());
}

QSize PTZDeviceListDelegate::sizeHint(const QStyleOptionViewItem &option, const QModelIndex &index) const
{
	auto size = QStyledItemDelegate::sizeHint(option, index);
	size.setWidth(25);
	size.setHeight(PTZControls::getInstance()->rowHeight());
	return size;
}

PTZDeviceListDelegate::CellLayout PTZDeviceListDelegate::layoutCell(const QModelIndex &index,
								    const QStyleOptionViewItem &option) const
{
	auto ptz = PTZControls::getInstance();
	QStyle *style = option.widget ? option.widget->style() : QApplication::style();
	CellLayout l;
	l.text = style->subElementRect(QStyle::SE_ItemViewItemText, &option, option.widget);
	l.lock = QRect();
	l.status = QRect();
	l.tally = QRect();

	l.iconMargin = qMax(0, (l.text.height() - iconSize()) / 2);
	const int iconBoxWidth = iconSize() + l.iconMargin * 2;

	const int tallySize = qMax(6, iconSize() / 2);
	l.tallyMargin = qMax(0, (l.text.height() - tallySize) / 2);
	const int tallyBoxWidth = tallySize + l.tallyMargin * 2;

	bool isLive = ptz->liveMoveLockActive() && index.data(PTZListModel::IsLiveRole).toBool();
	bool isConnected = index.data(PTZListModel::IsConnectedRole).toBool();

	l.tally = l.text.adjusted(0, 0, -(l.text.width() - tallyBoxWidth), 0);
	l.text.adjust(tallyBoxWidth, 0, 0, 0);

	if (isLive) {
		l.lock = l.text.adjusted(l.text.width() - iconBoxWidth, 0, 0, 0);
		l.text.adjust(0, 0, -iconBoxWidth, 0);
	}
	if (!isConnected) {
		l.status = l.text.adjusted(l.text.width() - iconBoxWidth, 0, 0, 0);
		l.text.adjust(0, 0, -iconBoxWidth, 0);
	}
	return l;
}

/**
 * Add icon on right hand side of drawing area
 */
void PTZDeviceListDelegate::paint(QPainter *painter, const QStyleOptionViewItem &option, const QModelIndex &index) const
{
	bool isLocked = index.data(PTZListModel::IsLockedRole).toBool();

	QStyleOptionViewItem opt(option);
	initStyleOption(&opt, index);

	/* Draw the background highlight */
	QStyle *style = opt.widget ? opt.widget->style() : QApplication::style();
	style->drawPrimitive(QStyle::PE_PanelItemViewItem, &opt, painter, opt.widget);

	/* Divide up the space into tally dot, the label, status icon and lock icon */
	CellLayout l = layoutCell(index, opt);

	if (l.lock.width()) {
		auto icon = isLocked ? &lockedIcon : &unlockedIcon;
		icon->paint(painter, l.lock.adjusted(l.iconMargin, 0, -l.iconMargin, 0));
	}
	if (l.status.width())
		disconnectedIcon.paint(painter, l.status.adjusted(l.iconMargin, 0, -l.iconMargin, 0));

	/* Tally: a colored visibility indictor - red for live, green for preview */
	const bool isLiveTally = index.data(PTZListModel::IsLiveRole).toBool();
	const bool isPreviewTally = index.data(PTZListModel::IsPreviewRole).toBool();
	if (isLiveTally || isPreviewTally) {
		QColor tallyColor = isLiveTally ? QColor(220, 50, 50) : QColor(60, 180, 60);
		painter->save();
		painter->setRenderHint(QPainter::Antialiasing);
		painter->setPen(Qt::NoPen);
		painter->setBrush(tallyColor);
		painter->drawEllipse(l.tally.adjusted(l.tallyMargin, l.tallyMargin, -l.tallyMargin, -l.tallyMargin));
		painter->restore();
	}

	/* Finally, render the text in the space remaining */
	style->drawItemText(painter, l.text, opt.displayAlignment, opt.palette, true, opt.text);
}

bool PTZDeviceListDelegate::editorEvent(QEvent *event, QAbstractItemModel *model, const QStyleOptionViewItem &option,
					const QModelIndex &index)
{
	if (!event || !model || !index.isValid())
		return false;

	const CellLayout l = layoutCell(index, option);
	QMouseEvent *mouseEvent = nullptr;
	QPoint pos;

	switch (event->type()) {
	case QEvent::MouseButtonRelease:
		mouseEvent = static_cast<QMouseEvent *>(event);
		pos = mouseEvent->pos();
		if (mouseEvent->button() == Qt::LeftButton && l.lock.contains(pos)) {
			bool isLocked = index.data(PTZListModel::IsLockedRole).toBool();
			model->setData(index, !isLocked, PTZListModel::IsLockedRole);
			return true;
		}
		break;
	case QEvent::MouseButtonDblClick:
		mouseEvent = static_cast<QMouseEvent *>(event);
		pos = mouseEvent->pos();
		if (mouseEvent->button() == Qt::LeftButton && l.text.contains(pos)) {
			ptz_settings_show(index);
			return true;
		}
		break;
	default:
		break;
	}
	return QStyledItemDelegate::editorEvent(event, model, option, index);
}

bool PTZDeviceListDelegate::helpEvent(QHelpEvent *event, QAbstractItemView *view, const QStyleOptionViewItem &option,
				      const QModelIndex &index)
{
	if (!event || !view || !index.isValid())
		return false;

	const CellLayout l = layoutCell(index, option);
	const QPoint pos = event->pos();

	if (l.lock.contains(pos)) {
		bool isLocked = index.data(PTZListModel::IsLockedRole).toBool();
		auto tooltip = isLocked ? obs_module_text("PTZ.Dock.Lock.Description")
					: obs_module_text("PTZ.Dock.Unlock.Description");
		QToolTip::showText(event->globalPos(), tooltip, view);
		return true;
	}

	if (l.status.contains(pos)) {
		QToolTip::showText(event->globalPos(), obs_module_text("PTZ.Device.Status.Disconnected"), view);
		return true;
	}

	return QStyledItemDelegate::helpEvent(event, view, option, index);
}

PTZPresetListDelegate::PTZPresetListDelegate(QObject *parent) : QStyledItemDelegate(parent)
{
	refreshTheme();
}

void PTZPresetListDelegate::refreshTheme()
{
	bool isDark = obs_frontend_is_theme_dark();
	recallIcon = QIcon(isDark ? "theme:Dark/refresh.svg" : ":res/images/refresh.svg");

	emit sizeHintChanged(QModelIndex());
}

QSize PTZPresetListDelegate::sizeHint(const QStyleOptionViewItem &option, const QModelIndex &index) const
{
	QSize size = QStyledItemDelegate::sizeHint(option, index);
	size.setHeight(PTZControls::getInstance()->rowHeight());
	return size;
}

PTZPresetListDelegate::CellLayout PTZPresetListDelegate::layoutCell(const QModelIndex &,
								    const QStyleOptionViewItem &option) const
{
	QStyle *style = option.widget ? option.widget->style() : QApplication::style();
	auto rect = style->subElementRect(QStyle::SE_ItemViewItemText, &option, option.widget);
	CellLayout l;

	/* Margin between icons & text tracks the height of the cell */
	l.iconMargin = qMax(0, (rect.height() - iconSize()) / 2);

	int iconBoxWidth = iconSize() + l.iconMargin * 2;
	l.text = rect.adjusted(0, 0, -iconBoxWidth, 0);
	l.recall = rect.adjusted(rect.width() - iconBoxWidth, 0, 0, 0);
	return l;
}

void PTZPresetListDelegate::paint(QPainter *painter, const QStyleOptionViewItem &option, const QModelIndex &index) const
{
	int textMargin = 2;
	QStyleOptionViewItem opt(option);
	initStyleOption(&opt, index);

	/* Draw the background highlight */
	QStyle *style = opt.widget ? opt.widget->style() : QApplication::style();
	style->drawPrimitive(QStyle::PE_PanelItemViewItem, &opt, painter, opt.widget);

	/* Divide up the space into the label and the recall button */
	CellLayout l = layoutCell(index, opt);
	QIcon::Mode iconMode = (opt.state & QStyle::State_Enabled) ? QIcon::Normal : QIcon::Disabled;
	recallIcon.paint(painter, l.recall.adjusted(0, l.iconMargin, 0, -l.iconMargin), Qt::AlignCenter, iconMode);
	style->drawItemText(painter, l.text.adjusted(textMargin, 0, 0, 0), opt.displayAlignment, opt.palette, true,
			    opt.text);
}

bool PTZPresetListDelegate::editorEvent(QEvent *event, QAbstractItemModel *model, const QStyleOptionViewItem &option,
					const QModelIndex &index)
{
	if (!event || !model || !index.isValid())
		return false;

	if (event->type() == QEvent::MouseButtonRelease) {
		auto mouseEvent = static_cast<QMouseEvent *>(event);
		const CellLayout l = layoutCell(index, option);
		if (mouseEvent->button() == Qt::LeftButton && l.recall.contains(mouseEvent->pos())) {
			uint32_t deviceId = index.parent().data(PTZListModel::DeviceIdRole).toUInt();
			int presetId = index.data(Qt::UserRole).toInt();
			ptzDeviceList.preset_recall(deviceId, presetId);
			return true;
		}
	}
	return QStyledItemDelegate::editorEvent(event, model, option, index);
}

bool PTZPresetListDelegate::helpEvent(QHelpEvent *event, QAbstractItemView *view, const QStyleOptionViewItem &option,
				      const QModelIndex &index)
{
	if (!event || !view || !index.isValid())
		return false;

	const CellLayout l = layoutCell(index, option);
	if (l.recall.contains(event->pos())) {
		QToolTip::showText(event->globalPos(), obs_module_text("PTZ.Preset.Recall.Tooltip"), view);
		return true;
	}

	return QStyledItemDelegate::helpEvent(event, view, option, index);
}
