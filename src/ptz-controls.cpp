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
#include <QPixmap>
#include <QCheckBox>
#include <QScrollBar>
#include <QLineEdit>
#include <QSlider>
#include <QToolButton>
#include <QMenu>
#include <QFileDialog>
#include <QMessageBox>

#include <qt-wrappers.hpp>
#include "touch-control.hpp"
#include "ui_ptz-controls.h"
#include "ptz-controls.hpp"
#include "ptz-list-model.hpp"
#include "ptz-thumbnail.hpp"
#include "ptz-legacy-migration.hpp"
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

void PTZControls::autoselectDevice(OBSSource scene)
{
	auto active_src_cb = [](obs_source_t *, obs_source_t *child, void *data) {
		auto index = static_cast<QModelIndex *>(data);
		if (!index->isValid())
			*index = ptzDeviceList->indexFromName(obs_source_get_name(child));
	};
	QModelIndex index = ptzDeviceList->indexFromName(obs_source_get_name(scene));
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
		ptzDeviceList->onSceneChanged();
		updateMoveControls();
		break;
	case OBS_FRONTEND_EVENT_STUDIO_MODE_ENABLED:
		if (autoselectEnabled()) {
			OBSSourceAutoRelease source = obs_frontend_get_current_scene();
			autoselectDevice(source.Get());
		}
		ptzDeviceList->onSceneChanged();
		updateMoveControls();
		break;
	case OBS_FRONTEND_EVENT_STUDIO_MODE_DISABLED:
	case OBS_FRONTEND_EVENT_PREVIEW_SCENE_CHANGED:
		if (autoselectEnabled() && obs_frontend_preview_program_mode_active()) {
			OBSSourceAutoRelease source = obs_frontend_get_current_preview_scene();
			autoselectDevice(source.Get());
		}
		ptzDeviceList->onSceneChanged();
		updateMoveControls();
		break;
	case OBS_FRONTEND_EVENT_FINISHED_LOADING:
		migrateLegacyDevices(true);
		ptz_thumbnail_sweep();
		break;
	case OBS_FRONTEND_EVENT_SCENE_COLLECTION_CHANGED:
		migrateLegacyDevices(false);
		break;
	case OBS_FRONTEND_EVENT_EXIT:
		/* OBS is shutting down. It has already run its own save pass (and
		 * so has called onFrontendSaveEvent()) as part of its shutdown
		 * sequence. The filters take their devices with them. */
		while (!hotkeys.isEmpty())
			obs_hotkey_unregister(hotkeys.takeFirst());
		obs_frontend_remove_event_callback(onFrontendEvent, this);
		obs_frontend_remove_save_callback(onFrontendSaveEvent, this);
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
	if (config_t *cfg = obs_frontend_get_user_config())
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
	emit themeRefreshed();
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

	refreshTheme();

	ui->deviceList->setModel(ptzDeviceList);
	deviceDelegate = new PTZDeviceListDelegate(ui->deviceList);
	ui->deviceList->setItemDelegate(deviceDelegate);
	connect(ptzDeviceList, &PTZListModel::dataChanged, this, &PTZControls::settingsChanged);

	copyActionsDynamicProperties();

	QItemSelectionModel *selectionModel = ui->deviceList->selectionModel();
	connect(selectionModel, &QItemSelectionModel::currentChanged, this, &PTZControls::currentChanged);

	presetDelegate = new PTZPresetListDelegate(ui->presetListView);
	ui->presetListView->setItemDelegate(presetDelegate);

	/* Add is a button with a menu when a device has both stores, and plain
	 * when it has one: the menu is only set then (presetUpdateActions()) */
	actionPresetAddCamera = new QAction(obs_module_text("PTZ.Action.Preset.AddCamera"), this);
	actionPresetAddCamera->setObjectName(QStringLiteral("actionPresetAddCamera"));
	actionPresetAddLocal = new QAction(obs_module_text("PTZ.Action.Preset.AddLocal"), this);
	actionPresetAddLocal->setObjectName(QStringLiteral("actionPresetAddLocal"));
	presetAddMenu = new QMenu(this);
	presetAddMenu->setObjectName(QStringLiteral("presetAddMenu"));
	presetAddMenu->addAction(actionPresetAddCamera);
	presetAddMenu->addAction(actionPresetAddLocal);
	/* What a click on Add itself does is a setting, which is also in the context menu */
	actionPresetDefaultCamera = new QAction(obs_module_text("PTZ.Settings.PresetDefaultCamera"), this);
	actionPresetDefaultCamera->setObjectName(QStringLiteral("actionPresetDefaultCamera"));
	actionPresetDefaultCamera->setCheckable(true);
	actionPresetDefaultCamera->setChecked(true);
	presetAddMenu->addSeparator();
	presetAddMenu->addAction(actionPresetDefaultCamera);
	connect(actionPresetAddCamera, &QAction::triggered, this, [this]() { presetAddTo(QStringLiteral("camera")); });
	connect(actionPresetAddLocal, &QAction::triggered, this, [this]() { presetAddTo(QStringLiteral("local")); });

	/* A slider on the preset toolbar sizes the grid's thumbnails; it is
	 * shown along with the grid */
	ui->presetListView->viewport()->installEventFilter(this);
	/* The dock has the one context menu: the movement controls' own would
	 * keep it from seeing right-clicks there */
	ui->movementControlsWidget->setOwnContextMenu(false);
	connect(this, &QWidget::customContextMenuRequested, this, &PTZControls::showContextMenu);
	ui->splitter->handle(1)->installEventFilter(this);
	presetZoomSlider = new QSlider(Qt::Horizontal, ui->presetToolbar);
	presetZoomSlider->setRange(PTZPresetListDelegate::minGridZoom, PTZPresetListDelegate::maxGridZoom);
	presetZoomSlider->setSingleStep(10);
	presetZoomSlider->setPageStep(25);
	presetZoomSlider->setValue(presetDelegate->gridZoom());
	presetZoomSlider->setFixedWidth(60);
	presetZoomSlider->setToolTip(obs_module_text("PTZ.Action.Preset.ThumbnailSize"));
	connect(presetZoomSlider, &QSlider::valueChanged, this, &PTZControls::setPresetGridZoom);
	/* An expanding spacer pushes the slider to the toolbar's right edge */
	auto *spacer = new QWidget(ui->presetToolbar);
	spacer->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);
	presetZoomSpacer = ui->presetToolbar->addWidget(spacer);
	presetZoomAction = ui->presetToolbar->addWidget(presetZoomSlider);
	presetZoomSpacer->setVisible(false);
	presetZoomAction->setVisible(false);
	updatePresetList();
	/* A model reset makes the views throw away their root index, and the
	 * camera list its selection, so the preset list has to be set up again.
	 * Queued, so that it happens after the views have done that. */
	connect(ptzDeviceList, &QAbstractItemModel::modelReset, this, &PTZControls::updatePresetList,
		Qt::QueuedConnection);

	joystickSetup();

	LoadConfig();

	obs_frontend_add_event_callback(onFrontendEvent, this);
	obs_frontend_add_save_callback(onFrontendSaveEvent, this);

	hide();

	auto movementButton = [this](const char *name) {
		return ui->movementControlsWidget->findChild<QToolButton *>(QString::fromLatin1(name));
	};

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
			ptzctrl->ui->movementControlsWidget->requestAutofocus(
				!ptzctrl->ui->movementControlsWidget->autofocusOn());
	};
	auto autofocusoncb = [](void *ptz_data, obs_hotkey_id, obs_hotkey_t *, bool pressed) {
		PTZControls *ptzctrl = static_cast<PTZControls *>(ptz_data);
		if (pressed)
			ptzctrl->ui->movementControlsWidget->requestAutofocus(true);
	};
	auto autofocusoffcb = [](void *ptz_data, obs_hotkey_id, obs_hotkey_t *, bool pressed) {
		PTZControls *ptzctrl = static_cast<PTZControls *>(ptz_data);
		if (pressed)
			ptzctrl->ui->movementControlsWidget->requestAutofocus(false);
	};
	registerHotkey("PTZ.PanTiltUpLeft", obs_module_text("PTZ.Action.PanTiltUpLeft"), cb,
		       movementButton("panTiltButton_upleft"));
	registerHotkey("PTZ.PanTiltLeft", obs_module_text("PTZ.Action.PanTiltLeft"), cb,
		       movementButton("panTiltButton_left"));
	registerHotkey("PTZ.PanTiltDownLeft", obs_module_text("PTZ.Action.PanTiltDownLeft"), cb,
		       movementButton("panTiltButton_downleft"));
	registerHotkey("PTZ.PanTiltUpRight", obs_module_text("PTZ.Action.PanTiltUpRight"), cb,
		       movementButton("panTiltButton_upright"));
	registerHotkey("PTZ.PanTiltRight", obs_module_text("PTZ.Action.PanTiltRight"), cb,
		       movementButton("panTiltButton_right"));
	registerHotkey("PTZ.PanTiltDownRight", obs_module_text("PTZ.Action.PanTiltDownRight"), cb,
		       movementButton("panTiltButton_downright"));
	registerHotkey("PTZ.PanTiltUp", obs_module_text("PTZ.Action.PanTiltUp"), cb,
		       movementButton("panTiltButton_up"));
	registerHotkey("PTZ.PanTiltDown", obs_module_text("PTZ.Action.PanTiltDown"), cb,
		       movementButton("panTiltButton_down"));
	registerHotkey("PTZ.ZoomWide", obs_module_text("PTZ.Action.ZoomWide"), cb, movementButton("zoomButton_wide"));
	registerHotkey("PTZ.ZoomTele", obs_module_text("PTZ.Action.ZoomTele"), cb, movementButton("zoomButton_tele"));
	registerHotkey("PTZ.FocusAutoToggle", obs_module_text("PTZ.Action.FocusAutoToggle"), autofocustogglecb, this);
	registerHotkey("PTZ.FocusAutoOn", obs_module_text("PTZ.Action.FocusAutoOn"), autofocusoncb, this);
	registerHotkey("PTZ.FocusAutoOff", obs_module_text("PTZ.Action.FocusAutoOff"), autofocusoffcb, this);
	registerHotkey("PTZ.FocusNear", obs_module_text("PTZ.Action.FocusNear"), cb, movementButton("focusButton_far"));
	registerHotkey("PTZ.FocusFar", obs_module_text("PTZ.Action.FocusFar"), cb, movementButton("focusButton_near"));
	registerHotkey("PTZ.FocusOneTouch", obs_module_text("PTZ.Action.FocusOneTouch"), cb,
		       movementButton("focusButton_onetouch"));
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
		auto row = ptzctrl->preset_hotkey_map[hotkey];
		if (pressed)
			ptzctrl->presetRecall(ptzctrl->presetIdAtRow(row));
	};

	auto preset_set_cb = [](void *ptz_data, obs_hotkey_id hotkey, obs_hotkey_t *, bool pressed) {
		PTZControls *ptzctrl = static_cast<PTZControls *>(ptz_data);
		auto row = ptzctrl->preset_hotkey_map[hotkey];
		if (pressed)
			ptzctrl->presetSet(ptzctrl->presetIdAtRow(row));
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
	ui->movementControlsWidget->setPanTilt(0, 0);
	ui->movementControlsWidget->setZoom(0);
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
		ui->movementControlsWidget->setPanTilt(readAxis(jd, joystick_pan_axis, joystick_pan_invert),
						       -readAxis(jd, joystick_tilt_axis, joystick_tilt_invert));
	if (updated & (1 << joystick_zoom_axis))
		ui->movementControlsWidget->setZoom(-readAxis(jd, joystick_zoom_axis, joystick_zoom_invert));
	if (updated & (1 << joystick_focus_axis))
		ui->movementControlsWidget->setFocus(-readAxis(jd, joystick_focus_axis, joystick_focus_invert));
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
	obs_data_set_bool(savedata, "onscreen_joystick_enabled", ui->movementControlsWidget->onscreenJoystick());
	obs_data_set_bool(savedata, "refresh_thumbnail_on_recall", refresh_thumbnail_on_recall);
	obs_data_set_int(savedata, "preset_grid_zoom", presetDelegate->gridZoom());
	obs_data_set_bool(savedata, "preset_grid_view", ui->actionPresetGridView->isChecked());
	obs_data_set_bool(savedata, "preset_default_camera", actionPresetDefaultCamera->isChecked());
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
		obs_data_set_string(
			savedata, "selected_device",
			QT_TO_UTF8(ui->deviceList->currentIndex().data(PTZListModel::DeviceUuidRole).toString()));

	/* Devices are saved with their filters. What is left here is those not
	 * yet migrated from the old self-managed backend */
	OBSDataArrayAutoRelease devices = ptz_legacy_devices_save();
	if (obs_data_array_count(devices))
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

	QString loadedFrom = QT_UTF8(file);
	OBSDataAutoRelease loaddata = obs_data_create_from_json_file_safe(file, "bak");
	if (!loaddata) {
		/* Try loading from the old configuration path */
		auto f = QString(file).replace("obs-ptz", "ptz-controls");
		loaddata = obs_data_create_from_json_file_safe(QT_TO_UTF8(f), "bak");
		if (loaddata)
			loadedFrom = f;
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
	obs_data_set_default_bool(loaddata, "refresh_thumbnail_on_recall", true);
	obs_data_set_default_int(loaddata, "preset_grid_zoom", 100);
	obs_data_set_default_bool(loaddata, "preset_grid_view", false);
	obs_data_set_default_bool(loaddata, "preset_default_camera", true);
	obs_data_set_default_bool(loaddata, "joystick_enable", false);
	obs_data_set_default_int(loaddata, "joystick_id", -1);
	obs_data_set_default_double(loaddata, "joystick_speed", 1.0);
	obs_data_set_default_double(loaddata, "joystick_deadzone", 0.0);

	live_move_lock_enabled = obs_data_get_bool(loaddata, "live_moves_disabled");
	autoselect_enabled = obs_data_get_bool(loaddata, "autoselect_enabled");
	speed_ramp_enabled = obs_data_get_bool(loaddata, "speed_ramp_enabled");
	ui->movementControlsWidget->setOnscreenJoystick(obs_data_get_bool(loaddata, "onscreen_joystick_enabled"));
	refresh_thumbnail_on_recall = obs_data_get_bool(loaddata, "refresh_thumbnail_on_recall");
	presetZoomSlider->setValue((int)obs_data_get_int(loaddata, "preset_grid_zoom"));
	ui->actionPresetGridView->setChecked(obs_data_get_bool(loaddata, "preset_grid_view"));
	actionPresetDefaultCamera->setChecked(obs_data_get_bool(loaddata, "preset_default_camera"));
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
		holdCameraColumnWidth();
	}

	const char *vertsplitterStateStr = obs_data_get_string(loaddata, "vertsplitter_state");
	if (vertsplitterStateStr) {
		QByteArray splitterState = QByteArray::fromBase64(QByteArray(vertsplitterStateStr));
		ui->vertsplitter->restoreState(splitterState);
	}

	/* The devices that config.json has are the old self-managed ones. They
	 * become filters once a scene collection is loaded, see migrateLegacyDevices() */
	ptz_legacy_load(QT_TO_UTF8(loadedFrom), loaddata);
	/* The camera that was selected: by id for the old self-managed devices, by
	 * the UUID of its filter since */
	legacy_current_selected = (uint32_t)obs_data_get_int(loaddata, "current_selected");
	selected_uuid = QT_UTF8(obs_data_get_string(loaddata, "selected_device"));
}

void PTZControls::migrateLegacyDevices(bool finishingLoading)
{
	if (ptz_legacy_migrate(finishingLoading))
		SaveConfig();
	if (legacy_current_selected) {
		const char *uuid = ptz_legacy_remap_id(legacy_current_selected);
		if (uuid) {
			selected_uuid = QT_UTF8(uuid);
			legacy_current_selected = 0;
		}
	}
	/* Once its device is there, whichever way it came to be */
	if (!selected_uuid.isEmpty()) {
		QModelIndex index = ptzDeviceList->indexFromUuid(selected_uuid);
		if (index.isValid()) {
			ui->deviceList->setCurrentIndex(index);
			selected_uuid.clear();
		}
	}
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

void PTZControls::setRefreshThumbnailOnRecall(bool enabled)
{
	if (enabled == refresh_thumbnail_on_recall)
		return;
	refresh_thumbnail_on_recall = enabled;
	emit refreshThumbnailOnRecallChanged(enabled);
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
	return ptzDeviceList->callDevice(ui->deviceList->currentIndex(), method, cd);
}

bool PTZControls::callCurrentDevice(const char *method, const char *arg, const QString &val) const
{
	calldata_t cd = {};
	calldata_set_string(&cd, arg, QT_TO_UTF8(val));
	bool called = callCurrentDevice(method, &cd);
	calldata_free(&cd);
	return called;
}

void PTZControls::updateMoveControls()
{
	auto device = ui->deviceList->currentIndex();
	bool is_locked = liveMoveLockActive() && device.data(PTZListModel::IsLockedRole).toBool();

	ui->deviceList->update();
	ui->presetListView->setEnabled(!is_locked && PTZListModel::hasFeature(device, "presets"));
	presetUpdateActions();

	RefreshToolBarStyling(ui->ptzToolbar);

	/* The movement controls, here and in the settings dialog, follow */
	emit moveControlsChanged();
}

void PTZControls::currentChanged(QModelIndex current, QModelIndex)
{
	/* Stops the camera that was being moved, if one was */
	ui->movementControlsWidget->setDevice(current);

	updatePresetList();
	updateMoveControls();
}

/**
 * Make the preset list show the presets of the selected camera.
 *
 * The preset list is a view onto the same tree model as the camera list, rooted
 * at the selected camera. A view rooted at an invalid index shows the top level
 * of its model, though, which is the list of cameras, so with no camera selected
 * there is nothing to root the preset list at. Take its model away instead, so
 * that it shows nothing.
 */
void PTZControls::updatePresetList()
{
	auto *view = ui->presetListView;
	auto device = ui->deviceList->currentIndex();

	if (device.isValid() != (view->model() == ptzDeviceList)) {
		view->setModel(device.isValid() ? ptzDeviceList : nullptr);
		/* The new model has a new selection model */
		disconnect(presetSelectionConnection);
		presetSelectionConnection = connect(view->selectionModel(), &QItemSelectionModel::currentChanged, this,
						    &PTZControls::presetUpdateActions);
	}
	if (device.isValid())
		view->setRootIndex(device);
	presetUpdateActions();
}

void PTZControls::settingsChanged(const QModelIndex &topLeft, const QModelIndex &bottomRight)
{
	auto index = ui->deviceList->currentIndex();
	QItemSelectionRange range(topLeft, bottomRight);
	if (range.contains(index))
		updateMoveControls();
}

void PTZControls::presetSet(const QString &preset_id)
{
	if (!preset_id.isEmpty())
		callCurrentDevice("ptz_preset_save", "id", preset_id);
}

void PTZControls::presetRecall(const QString &preset_id)
{
	if (!preset_id.isEmpty())
		callCurrentDevice("ptz_preset_recall", "id", preset_id);
}

QString PTZControls::presetIndexToId(QModelIndex index) const
{
	return index.isValid() ? index.data(Qt::UserRole).toString() : QString();
}

QString PTZControls::presetIdAtRow(int row) const
{
	auto device = ui->deviceList->currentIndex();
	return device.isValid() ? presetIndexToId(ptzDeviceList->index(row, 0, device)) : QString();
}

/* The menu's arrow is part of the Add button. The theme's toolbar rule sizes a
 * button for one icon and boxes it, which leaves the arrow's area outside
 * the box, square and unhighlighted. So this one has the width of a
 * button for one icon, which has the same space as theirs either side of
 * its icon, and the arrow's area as well, inside the box: the arrow in the
 * middle of it, with a line, 1px, between it and the icon. The line is in
 * the colour of the border and is there while the button is highlighted, as
 * the border is: both are the palette's, so they match. The area is the
 * button's right padding, which the arrow's half of it fills. */
void PTZControls::updatePresetAddButton()
{
	/* %1 is the contents' width, %2 the arrow's area, %3 the arrow's image */
	const QString css = QStringLiteral(
		"QToolButton { min-width: %1px; max-width: %1px; padding-left: 0px; padding-right: %2px; }"
		"QToolButton[highlighted=\"true\"] { border-color: palette(light); }"
		"QToolButton::menu-button { border: none; border-left: 1px solid transparent; margin: 3px 0px; width: %2px; }"
		"QToolButton[highlighted=\"true\"]::menu-button { border-left-color: palette(light); }"
		"QToolButton::menu-arrow { image: url(%3); width: 8px; height: 8px; }");
	QWidget *sibling = ui->presetToolbar->widgetForAction(ui->actionPresetMoveUp);
	if (!presetAddButton)
		return;
	QString style;
	if (presetAddSplit) {
		/* The width a plain button was given, once it has one */
		if (!sibling || !sibling->isVisible() || sibling->width() <= 0)
			return;
		const int area = 17;
		const int wanted = sibling->width() + area;
		/* A stylesheet's width is the contents' only, with the border, padding and
		 * margin outside it, so it is found by trying: what it was, moved by how far
		 * the button then is from the width it should have. It is settled when the
		 * two agree. */
		if (presetAddContent <= 0)
			presetAddContent = wanted - area - 4;
		else if (presetAddButton->width() != wanted && presetAddButton->styleSheet() != QString())
			presetAddContent += wanted - presetAddButton->width();
		QString arrow = obs_frontend_is_theme_dark() ? QStringLiteral(":/icons/icons/preset_menu_dark.svg")
							     : QStringLiteral(":/icons/icons/preset_menu_light.svg");
		style = css.arg(presetAddContent).arg(area).arg(arrow);
	} else {
		presetAddContent = 0;
	}
	if (presetAddButton->styleSheet() != style) {
		presetAddButton->setStyleSheet(style);
		/* ...and the button is measured again once it has been laid out */
		if (presetAddSplit)
			QTimer::singleShot(0, this, &PTZControls::updatePresetAddButton);
	}
}

void PTZControls::presetUpdateActions()
{
	auto presetIndex = ui->presetListView->currentIndex();
	auto deviceIndex = ui->deviceList->currentIndex();
	int count = ptzDeviceList->rowCount(deviceIndex);
	bool presets = PTZListModel::hasFeature(deviceIndex, "presets");
	bool isValid = presetIndex.isValid() && presets;
	ui->actionPresetAdd->setEnabled(presets);
	/* The menu is only for a device with both stores, and the button's
	 * click makes a preset in the one chosen last */
	QStringList stores = presets ? ptzDeviceList->presetStores(deviceIndex) : QStringList();
	bool both = stores.contains(QStringLiteral("camera")) && stores.contains(QStringLiteral("local"));
	if (auto *button = qobject_cast<QToolButton *>(ui->presetToolbar->widgetForAction(ui->actionPresetAdd))) {
		if (both != (button->menu() != nullptr))
			button->setMenu(both ? presetAddMenu : nullptr);
		if (!presetAddButton) {
			presetAddButton = button;
			button->installEventFilter(this);
		}
		button->setPopupMode(both ? QToolButton::MenuButtonPopup : QToolButton::DelayedPopup);
		/* Its stylesheet needs the size the toolbar gives the buttons beside it,
		 * which is not known until it has laid them out */
		presetAddSplit = both;
		if (QWidget *sibling = ui->presetToolbar->widgetForAction(ui->actionPresetMoveUp))
			sibling->installEventFilter(this);
		QTimer::singleShot(0, this, &PTZControls::updatePresetAddButton);
	}
	/* Only a device with both stores has a choice of where Add goes */
	actionPresetDefaultCamera->setEnabled(both);
	actionPresetAddCamera->setEnabled(presets && stores.contains(QStringLiteral("camera")));
	actionPresetAddLocal->setEnabled(presets && stores.contains(QStringLiteral("local")));
	ui->actionPresetRemove->setEnabled(isValid);
	ui->actionPresetMoveUp->setEnabled(isValid && count > 1 && presetIndex.row() > 0);
	ui->actionPresetMoveDown->setEnabled(isValid && count > 1 && presetIndex.row() < count - 1);
	ui->actionPresetExport->setEnabled(presets && count > 0);
	ui->actionPresetImport->setEnabled(presets);
	RefreshToolBarStyling(ui->presetToolbar);
}

void PTZControls::on_presetListView_activated(QModelIndex index)
{
	presetRecall(presetIndexToId(index));
}

/* The dock's one context menu. Every right-click that nothing below has
 * accepted comes here, disabled widgets included, which pass it on. Where it
 * landed adds what is for that camera, preset or control at the top, and the
 * settings that are the dock's own follow wherever it was. */
void PTZControls::showContextMenu(const QPoint &pos)
{
	QWidget *under = childAt(pos);
	auto within = [under](QWidget *widget) {
		return under && (under == widget || widget->isAncestorOf(under));
	};
	QMenu menu;

	/* A camera: its power and white balance */
	QModelIndex device;
	QAction *powerAction = nullptr;
	QAction *wbOnetouchAction = nullptr;
	bool power_on = false;
	if (within(ui->deviceList))
		device = ui->deviceList->indexAt(ui->deviceList->viewport()->mapFrom(this, pos));
	if (device.isValid()) {
		OBSDataAutoRelease state = obs_data_create();
		ptzDeviceList->saveState(device, state.Get());
		power_on = obs_data_get_bool(state, "power_on");
		if (PTZListModel::hasFeature(device, "power"))
			powerAction = menu.addAction(
				obs_module_text(power_on ? "PTZ.Action.PowerOff" : "PTZ.Action.PowerOn"));

		/* only in the one-push white balance mode */
		bool wb_onepush = (obs_data_get_int(state, "wb_mode") == 3);
		if (wb_onepush && PTZListModel::hasFeature(device, "wb_onepush"))
			wbOnetouchAction = menu.addAction(obs_module_text("PTZ.Action.WhiteBalance.OnePushTrigger"));
		if (powerAction || wbOnetouchAction)
			menu.addSeparator();
	}

	/* The presets: what acts on the one clicked on, which a locked camera's
	 * disabled list doesn't select, so it is left out then, and what acts on
	 * the list */
	if (within(ui->presetListView)) {
		QPoint viewPos = ui->presetListView->viewport()->mapFrom(this, pos);
		if (ui->presetListView->isEnabled() && ui->presetListView->indexAt(viewPos).isValid()) {
			menu.addAction(ui->actionPresetRename);
			menu.addAction(ui->actionPresetSave);
			menu.addAction(ui->actionPresetRemove);
			menu.addSeparator();
		}
		if (actionPresetAddCamera->isEnabled() && actionPresetAddLocal->isEnabled()) {
			menu.addAction(actionPresetAddCamera);
			menu.addAction(actionPresetAddLocal);
		} else {
			menu.addAction(ui->actionPresetAdd);
		}
		menu.addAction(ui->actionPresetExport);
		menu.addAction(ui->actionPresetImport);
		menu.addSeparator();
	}

	/* A movement control: Set Home */
	int controlsActions = menu.actions().size();
	if (within(ui->movementControlsWidget)) {
		ui->movementControlsWidget->addContextActions(&menu, ui->movementControlsWidget->mapFrom(this, pos));
		if (menu.actions().size() > controlsActions)
			menu.addSeparator();
	}

	/* The dock's own settings */
	QAction *autoselectAction = menu.addAction(obs_module_text("PTZ.Settings.CameraAutoselect"));
	autoselectAction->setCheckable(true);
	autoselectAction->setChecked(autoselectEnabled());
	connect(autoselectAction, &QAction::toggled, this, &PTZControls::setAutoselectEnabled);
	if (obs_frontend_preview_program_mode_active()) {
		QAction *blockliveAction = menu.addAction(obs_module_text("PTZ.Settings.BlockLiveMoves"));
		blockliveAction->setCheckable(true);
		blockliveAction->setChecked(liveMoveLockEnabled());
		connect(blockliveAction, &QAction::toggled, this, &PTZControls::setLiveMoveLockEnabled);
	}
	QAction *refreshAction = menu.addAction(obs_module_text("PTZ.Preset.RefreshThumbnailOnRecall"));
	refreshAction->setCheckable(true);
	refreshAction->setChecked(refresh_thumbnail_on_recall);
	connect(refreshAction, &QAction::toggled, this, &PTZControls::setRefreshThumbnailOnRecall);
	menu.addAction(actionPresetDefaultCamera);
	menu.addAction(ui->actionPresetGridView);
	menu.addSeparator();

	if (!ui->movementControlsWidget->isHidden() && !within(ui->movementControlsWidget)) {
		/* Over the controls the joystick toggle is already above */
		QAction *joystickAction = menu.addAction(obs_module_text("PTZ.Dock.OnscreenJoystick"));
		joystickAction->setCheckable(true);
		joystickAction->setChecked(ui->movementControlsWidget->onscreenJoystick());
		connect(joystickAction, &QAction::toggled, ui->movementControlsWidget,
			&PTZMovementControls::setOnscreenJoystick);
	}
	menu.addAction(ui->actionProperties);

	QAction *action = menu.exec(mapToGlobal(pos));
	if (action == nullptr)
		return;
	if (action == powerAction) {
		OBSDataAutoRelease request = obs_data_create();
		obs_data_set_bool(request, "power_on", !power_on);
		ptzDeviceList->setState(device, request.Get());
	} else if (action == wbOnetouchAction) {
		calldata cd = {};
		calldata_set_string(&cd, "name", "wb_onepush");
		ptzDeviceList->callDevice(device, "ptz_trigger", &cd);
		calldata_free(&cd);
	}
}

void PTZControls::on_actionProperties_triggered()
{
	ptz_settings_show(ui->deviceList->currentIndex());
}

void PTZControls::on_actionPresetAdd_triggered()
{
	/* The one store a device has, or the default if it has both */
	bool camera = actionPresetAddCamera->isEnabled();
	bool local = actionPresetAddLocal->isEnabled();
	if (camera && local)
		presetAddTo(actionPresetDefaultCamera->isChecked() ? QStringLiteral("camera")
								   : QStringLiteral("local"));
	else
		presetAddTo(local ? QStringLiteral("local") : QStringLiteral("camera"));
}

void PTZControls::presetAddTo(const QString &store)
{
	auto parent = ui->deviceList->currentIndex();
	if (!parent.isValid())
		return;
	QString id = ptzDeviceList->addPreset(parent.data(PTZListModel::DeviceUuidRole).toString(), store);
	for (int row = 0; !id.isEmpty() && row < ptzDeviceList->rowCount(parent); row++) {
		QModelIndex index = ptzDeviceList->index(row, 0, parent);
		if (presetIndexToId(index) != id)
			continue;
		ui->presetListView->setCurrentIndex(index);
		ui->presetListView->edit(index);
		break;
	}
	presetUpdateActions();
}

void PTZControls::on_actionPresetRemove_triggered()
{
	auto index = ui->presetListView->currentIndex();
	if (!index.isValid())
		return;
	ptzDeviceList->removeRows(index.row(), 1, ui->deviceList->currentIndex());
	presetUpdateActions();
}

void PTZControls::on_actionPresetMoveUp_triggered()
{
	auto index = ui->presetListView->currentIndex();
	auto parent = ui->deviceList->currentIndex();
	if (!index.isValid())
		return;
	ptzDeviceList->moveRow(parent, index.row(), parent, index.row() - 1);
	presetUpdateActions();
}

void PTZControls::on_actionPresetMoveDown_triggered()
{
	auto index = ui->presetListView->currentIndex();
	auto parent = ui->deviceList->currentIndex();
	if (!index.isValid())
		return;
	ptzDeviceList->moveRow(parent, index.row(), parent, index.row() + 2);
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

/* The preset list is either a column of rows, or a wrapping grid of cells */
void PTZControls::on_actionPresetGridView_toggled(bool checked)
{
	auto *view = ui->presetListView;
	presetDelegate->setGridMode(checked);
	if (checked) {
		view->setViewMode(QListView::IconMode);
		/* IconMode's defaults let the user drag the cells around */
		view->setMovement(QListView::Static);
		view->setResizeMode(QListView::Adjust);
		view->setUniformItemSizes(true);
		view->setSpacing(PTZPresetListDelegate::gridSpacing);
		view->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
	} else {
		view->setViewMode(QListView::ListMode);
		view->setSpacing(0);
		view->setHorizontalScrollBarPolicy(Qt::ScrollBarAsNeeded);
	}
	presetZoomSpacer->setVisible(checked);
	presetZoomAction->setVisible(checked);
	view->doItemsLayout();
}

/* The grid's columns depend on the view's width, so lay it out again when that changes */
/* The camera column, with the movement controls, keeps its width when the
 * dock is resized: giving the preset column all of the splitter's stretch
 * does that when the dock grows, but a splitter that has to give up width
 * takes it from every child that can spare any. So the column's minimum
 * width is the width it has, and the preset list is what shrinks. */
void PTZControls::holdCameraColumnWidth()
{
	ui->cameraColumn->setMinimumWidth(qMax(0, ui->splitter->sizes().value(0)));
}

bool PTZControls::eventFilter(QObject *watched, QEvent *event)
{
	if (watched == ui->presetToolbar->widgetForAction(ui->actionPresetMoveUp) &&
	    (event->type() == QEvent::Resize || event->type() == QEvent::Show))
		QTimer::singleShot(0, this, &PTZControls::updatePresetAddButton);
	/* Whether the Add button is highlighted, for its stylesheet, which can't
	 * say that of the arrow's half of it */
	if (watched == presetAddButton && (event->type() == QEvent::Enter || event->type() == QEvent::Leave)) {
		bool highlighted = event->type() == QEvent::Enter;
		presetAddButton->setProperty("highlighted", highlighted);
		presetAddButton->style()->unpolish(presetAddButton);
		presetAddButton->style()->polish(presetAddButton);
	}
	/* The minimum has to come off while the splitter handle is dragged, or
	 * the column could only be made wider */
	if (watched == ui->splitter->handle(1)) {
		if (event->type() == QEvent::MouseButtonPress)
			ui->cameraColumn->setMinimumWidth(0);
		else if (event->type() == QEvent::MouseButtonRelease)
			holdCameraColumnWidth();
	}
	if (watched == ui->presetListView->viewport() && event->type() == QEvent::Resize &&
	    presetDelegate->gridMode() && presetDelegate->layoutWidthChanged(ui->presetListView)) {
		/* Not from here: laying out can resize the viewport again */
		QMetaObject::invokeMethod(
			this,
			[this] {
				emit presetDelegate->sizeHintChanged(QModelIndex());
				ui->presetListView->doItemsLayout();
			},
			Qt::QueuedConnection);
	}
	return QFrame::eventFilter(watched, event);
}

void PTZControls::setPresetGridZoom(int percent)
{
	presetDelegate->setGridZoom(percent);
	ui->presetListView->doItemsLayout();
}

void PTZControls::on_actionPresetExport_triggered(QString filename)
{
	auto fileExtension = QString("%1 (*.json)").arg(obs_module_text("PTZ.Preset.FileFilter"));
	QModelIndex index = ui->deviceList->currentIndex();
	if (!index.isValid())
		return;

	QString deviceName = ptzDeviceList->data(index, Qt::DisplayRole).toString();
	QString defaultName = QString(deviceName).replace(QLatin1Char('/'), QLatin1Char('_')) + " presets.json";
	if (filename.isEmpty())
		filename = QFileDialog::getSaveFileName(this, obs_module_text("PTZ.Action.Preset.Export"), defaultName,
							fileExtension);
	if (filename.isEmpty())
		return;

	/* save() serializes the device's whole config; presets/preset_max are
	 * just the subset of that we actually want in the exported file. */
	OBSDataAutoRelease fullConfig = obs_data_create();
	ptzDeviceList->save(index, fullConfig.Get());

	OBSDataAutoRelease data = obs_data_create();
	obs_data_set_int(data, "obs-ptz-preset-format", 1);
	obs_data_set_string(data, "device", QT_TO_UTF8(deviceName));
	obs_data_set_int(data, "preset_max", obs_data_get_int(fullConfig, "preset_max"));
	OBSDataArrayAutoRelease presets = obs_data_get_array(fullConfig, "presets");
	/* Thumbnails are files on this machine, so they don't go in the export.
	 * save() gave us a private copy of the presets, safe to edit. */
	for (size_t i = 0; presets && i < obs_data_array_count(presets); i++) {
		OBSDataAutoRelease item = obs_data_array_item(presets, i);
		obs_data_unset_user_value(item, "thumbnail");
	}
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
	QString deviceUuid = ptzDeviceList->data(index, PTZListModel::DeviceUuidRole).toString();

	/* Merge just the presets/preset_max subset from the imported file
	 * into the device's current full config, then update() with that --
	 * update() takes a complete settings object, and would otherwise
	 * read every other setting (pan/tilt speed, invert flags, ...) as
	 * unset, since this file only ever has the two preset-related
	 * keys. */
	OBSDataAutoRelease fullConfig = obs_data_create();
	ptzDeviceList->save(index, fullConfig.Get());
	if (obs_data_has_user_value(data, "preset_max"))
		obs_data_set_int(fullConfig, "preset_max", obs_data_get_int(data, "preset_max"));
	OBSDataArrayAutoRelease presets = obs_data_get_array(data, "presets");
	obs_data_set_array(fullConfig, "presets", presets);

	ptzDeviceList->update(index, fullConfig.Get());
	ptzDeviceList->do_reset();
	/* restore selection after reset */
	ui->deviceList->setCurrentIndex(ptzDeviceList->indexFromUuid(deviceUuid));
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
	poweredOffIcon = QIcon(isDark ? ":/icons/icons/power_dark.svg" : ":/icons/icons/power_light.svg");
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

	/* The disconnected indicator shares the tally slot instead of taking its own */
	if (!isConnected || index.data(PTZListModel::IsPoweredOffRole).toBool())
		l.status = l.tally;

	if (isLive) {
		l.lock = l.text.adjusted(l.text.width() - iconBoxWidth, 0, 0, 0);
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

	/* Divide up the space into tally dot (or status icon), the label and lock icon */
	CellLayout l = layoutCell(index, opt);

	if (l.lock.width()) {
		auto icon = isLocked ? &lockedIcon : &unlockedIcon;
		icon->paint(painter, l.lock.adjusted(l.iconMargin, 0, -l.iconMargin, 0));
	}

	/* Tally: a colored visibility indictor - red for live, green for preview */
	const bool isLiveTally = index.data(PTZListModel::IsLiveRole).toBool();
	const bool isPreviewTally = index.data(PTZListModel::IsPreviewRole).toBool();
	if (l.status.width()) {
		const QRect r = l.status.adjusted(l.iconMargin, 0, -l.iconMargin, 0);
		const bool connected = index.data(PTZListModel::IsConnectedRole).toBool();
		(connected ? poweredOffIcon : disconnectedIcon).paint(painter, r);
	} else if (isLiveTally || isPreviewTally) {
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
		const bool connected = index.data(PTZListModel::IsConnectedRole).toBool();
		QToolTip::showText(event->globalPos(),
				   obs_module_text(connected ? "PTZ.Device.Status.PoweredOff"
							     : "PTZ.Device.Status.Disconnected"),
				   view);
		return true;
	}

	return QStyledItemDelegate::helpEvent(event, view, option, index);
}

static constexpr int thumbnailMargin = 1;

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

void PTZPresetListDelegate::setGridMode(bool grid)
{
	if (m_gridMode == grid)
		return;
	m_gridMode = grid;
	emit sizeHintChanged(QModelIndex());
}

/* A grid cell is just a 16:9 thumbnail, with the name drawn over it */
/* The width QListView lays its cells out in. It leaves room for the vertical
 * scrollbar whether or not it is showing, so that the scrollbar appearing
 * doesn't change the layout, and neither should this. */
int PTZPresetListDelegate::layoutWidth(const QAbstractScrollArea *view)
{
	QScrollBar *vbar = view->verticalScrollBar();
	QStyle *style = view->style();
	int width = view->maximumViewportSize().width();
	if (view->verticalScrollBarPolicy() == Qt::ScrollBarAsNeeded &&
	    !style->pixelMetric(QStyle::PM_ScrollView_ScrollBarOverlap, nullptr, vbar)) {
		width -= style->pixelMetric(QStyle::PM_ScrollBarExtent, nullptr, vbar);
		if (style->styleHint(QStyle::SH_ScrollView_FrameOnlyAroundContents))
			width -= style->pixelMetric(QStyle::PM_DefaultFrameWidth, nullptr, view) * 2;
	}
	return width;
}

bool PTZPresetListDelegate::layoutWidthChanged(const QAbstractScrollArea *view)
{
	int width = layoutWidth(view);
	if (width == m_lastLayoutWidth)
		return false;
	m_lastLayoutWidth = width;
	return true;
}

int PTZPresetListDelegate::gridCellWidth(const QWidget *view) const
{
	int target = PTZControls::getInstance()->rowHeight() * 3 * m_gridZoom / 100;
	auto *area = qobject_cast<const QAbstractScrollArea *>(view);
	if (!area)
		return target;
	/* Cells sit gridSpacing from each other and from the left edge, and the
	 * view wraps a cell whose right edge, plus a gridSpacing, reaches the
	 * layout's last pixel */
	int available = layoutWidth(area) - 1 - gridSpacing;
	int columns = qMax(1, available / (target + gridSpacing));
	return qMax(1, (available - columns * gridSpacing) / columns);
}

void PTZPresetListDelegate::setGridZoom(int percent)
{
	percent = std::clamp(percent, minGridZoom, maxGridZoom);
	if (m_gridZoom == percent)
		return;
	m_gridZoom = percent;
	emit sizeHintChanged(QModelIndex());
}

QSize PTZPresetListDelegate::sizeHint(const QStyleOptionViewItem &option, const QModelIndex &index) const
{
	QSize size = QStyledItemDelegate::sizeHint(option, index);
	int rowHeight = PTZControls::getInstance()->rowHeight();
	if (m_gridMode) {
		int cellWidth = gridCellWidth(option.widget);
		int thumbWidth = cellWidth - 2 * thumbnailMargin;
		return QSize(cellWidth, thumbWidth * 9 / 16 + 2 * thumbnailMargin);
	}
	size.setHeight(rowHeight);
	return size;
}

/* In the grid the name is edited over the thumbnail, in the band it is drawn
 * in, rather than the editor covering the whole cell and hiding the picture */
QWidget *PTZPresetListDelegate::createEditor(QWidget *parent, const QStyleOptionViewItem &option,
					     const QModelIndex &index) const
{
	QWidget *editor = QStyledItemDelegate::createEditor(parent, option, index);
	if (m_gridMode) {
		/* The name drawn underneath would show through the editor */
		m_editing = index;
		if (auto *lineEdit = qobject_cast<QLineEdit *>(editor)) {
			lineEdit->setFrame(false);
			lineEdit->setAlignment(Qt::AlignCenter);
			/* The same translucent band, and white text, as when it isn't being edited */
			lineEdit->setStyleSheet("QLineEdit { background: rgba(0, 0, 0, 160); color: white; "
						"border: 0; padding: 0; margin: 0; }");
		}
	}
	return editor;
}

void PTZPresetListDelegate::destroyEditor(QWidget *editor, const QModelIndex &index) const
{
	m_editing = QModelIndex();
	QStyledItemDelegate::destroyEditor(editor, index);
}

void PTZPresetListDelegate::updateEditorGeometry(QWidget *editor, const QStyleOptionViewItem &option,
						 const QModelIndex &index) const
{
	if (!m_gridMode) {
		QStyledItemDelegate::updateEditorGeometry(editor, option, index);
		return;
	}
	editor->setGeometry(layoutCell(index, option).text);
}

PTZPresetListDelegate::CellLayout PTZPresetListDelegate::layoutCell(const QModelIndex &,
								    const QStyleOptionViewItem &option) const
{
	CellLayout l;

	if (m_gridMode) {
		QRect cell = option.rect.adjusted(thumbnailMargin, thumbnailMargin, -thumbnailMargin, -thumbnailMargin);
		l.thumbnail = QRect(cell.left(), cell.top(), cell.width(), cell.width() * 9 / 16);
		/* The name is a band along the bottom of the thumbnail */
		int textHeight = qMin(l.thumbnail.height(), option.fontMetrics.height() + 2);
		l.text = QRect(l.thumbnail.left(), l.thumbnail.bottom() + 1 - textHeight, l.thumbnail.width(),
			       textHeight);
		/* The recall button sits over the thumbnail's top corner */
		l.iconMargin = 2;
		int box = iconSize() + l.iconMargin * 2;
		l.recall = QRect(l.thumbnail.right() - box + 1, l.thumbnail.top(), box, box);
		return l;
	}

	QStyle *style = option.widget ? option.widget->style() : QApplication::style();
	auto rect = style->subElementRect(QStyle::SE_ItemViewItemText, &option, option.widget);

	/* Margin between icons & text tracks the height of the cell */
	l.iconMargin = qMax(0, (rect.height() - iconSize()) / 2);

	int iconBoxWidth = iconSize() + l.iconMargin * 2;
	/* The thumbnail is as tall as the row allows */
	int thumbHeight = qMax(0, rect.height() - 2 * thumbnailMargin);
	int thumbWidth = thumbHeight * 16 / 9;
	l.thumbnail = QRect(rect.left(), rect.top() + thumbnailMargin, thumbWidth, thumbHeight);
	l.text = rect.adjusted(thumbWidth + thumbnailMargin * 2, 0, -iconBoxWidth, 0);
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

	/* Divide up the space into the thumbnail, label and the recall button */
	CellLayout l = layoutCell(index, opt);
	QIcon::Mode iconMode = (opt.state & QStyle::State_Enabled) ? QIcon::Normal : QIcon::Disabled;

	/* The thumbnail, letterboxed into its 16:9 box, or a blank one */
	painter->save();
	painter->fillRect(l.thumbnail, opt.palette.color(QPalette::Mid));
	QPixmap thumbnail = index.data(PTZListModel::ThumbnailRole).value<QPixmap>();
	if (!thumbnail.isNull()) {
		QSize size = thumbnail.size().scaled(l.thumbnail.size(), Qt::KeepAspectRatio);
		QRect target(QPoint(0, 0), size);
		target.moveCenter(l.thumbnail.center());
		painter->setRenderHint(QPainter::SmoothPixmapTransform);
		painter->drawPixmap(target, thumbnail);
	}
	painter->restore();

	if (m_gridMode) {
		/* A backdrop keeps the recall icon visible over any picture */
		painter->save();
		painter->setRenderHint(QPainter::Antialiasing);
		painter->setPen(Qt::NoPen);
		painter->setBrush(QColor(0, 0, 0, 128));
		painter->drawEllipse(l.recall);
		painter->restore();
		recallIcon.paint(painter, l.recall.adjusted(l.iconMargin, l.iconMargin, -l.iconMargin, -l.iconMargin),
				 Qt::AlignCenter, iconMode);
		/* The name, in white on a translucent band, unless the editor is there */
		if (QModelIndex(m_editing) == index)
			return;
		painter->fillRect(l.text, QColor(0, 0, 0, 128));
		QString text = opt.fontMetrics.elidedText(opt.text, Qt::ElideRight, l.text.width() - 2 * textMargin);
		painter->save();
		painter->setPen(Qt::white);
		painter->drawText(l.text, Qt::AlignCenter, text);
		painter->restore();
		return;
	}

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
			QString deviceUuid = index.parent().data(PTZListModel::DeviceUuidRole).toString();
			QString presetId = index.data(Qt::UserRole).toString();
			ptzDeviceList->preset_recall(deviceUuid, presetId);
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
