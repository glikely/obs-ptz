/* Pan Tilt Zoom camera movement controls
 *
 * Copyright 2020-2026 Grant Likely <grant.likely@secretlab.ca>
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include <algorithm>
#include <cmath>
#include <obs-module.h>
#include <obs.hpp>
#include <obs-frontend-api.h>
#include <QGuiApplication>
#include <QIcon>
#include <QItemSelectionRange>
#include <QContextMenuEvent>
#include <QMenu>
#include <QToolButton>
#include <QResizeEvent>

#include "ptz-movement-controls.hpp"
#include "ui_ptz-movement-controls.h"
#include "ptz-controls.hpp"
#include "ptz-list-model.hpp"
#include "ptz.h"

/**
 * class squareResizeFilter - Event filter to adjust button minimum height
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

PTZMovementControls::PTZMovementControls(QWidget *parent) : QWidget(parent), ui(new Ui::PTZMovementControls)
{
	ui->setupUi(this);
	m_margins = ui->movementControlsGridLayout->contentsMargins();

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
		setStyleSheet("margin-left: 0px; margin-right: 0px");

	refreshTheme();

	connect(&accel_timer, &QTimer::timeout, this, &PTZMovementControls::accelTimerHandler);
	connect(ui->panTiltTouch, &TouchControl::positionChanged, [this](double p, double t) { setPanTilt(p, t); });

	/* What the controls may do follows the device, and the dock's settings
	 * that lock a live camera. The dock says when those or the theme change,
	 * having brought the list model up to date first. */
	connect(ptzDeviceList, &PTZListModel::dataChanged, this,
		[this](const QModelIndex &topLeft, const QModelIndex &bottomRight) {
			if (QItemSelectionRange(topLeft, bottomRight).contains(device()))
				updateControls();
		});
	if (auto controls = PTZControls::getInstance()) {
		connect(controls, &PTZControls::moveControlsChanged, this, &PTZMovementControls::updateControls);
		connect(controls, &PTZControls::themeRefreshed, this, &PTZMovementControls::refreshTheme);
	}
	updateControls();

	/* Keep the buttons square */
	m_squareFilter = new squareResizeFilter(this);
	installEventFilter(m_squareFilter);
	ui->pantiltStack->installEventFilter(m_squareFilter);
}

PTZMovementControls::~PTZMovementControls() = default;

/* The theme has changed; the button icons come in a light and a dark variant */
void PTZMovementControls::refreshTheme()
{
	const char *variant = obs_frontend_is_theme_dark() ? "dark" : "light";
	const QList<std::pair<QAbstractButton *, const char *>> buttons = {
		{ui->panTiltButton_upleft, "pantilt_upleft"},
		{ui->panTiltButton_up, "pantilt_up"},
		{ui->panTiltButton_upright, "pantilt_upright"},
		{ui->panTiltButton_left, "pantilt_left"},
		{ui->panTiltButton_home, "pantilt_home"},
		{ui->panTiltButton_right, "pantilt_right"},
		{ui->panTiltButton_downleft, "pantilt_downleft"},
		{ui->panTiltButton_down, "pantilt_down"},
		{ui->panTiltButton_downright, "pantilt_downright"},
		{ui->zoomButton_tele, "zoom_in"},
		{ui->zoomButton_wide, "zoom_out"},
		{ui->focusButton_auto, "focus_auto"},
		{ui->focusButton_near, "focus_near"},
		{ui->focusButton_far, "focus_far"},
	};
	for (const auto &[button, name] : buttons)
		button->setIcon(QIcon(QString(":/icons/icons/%1_%2.svg").arg(name, variant)));

	/* The theme's density may have changed too */
	applySize();
}

void PTZMovementControls::setThemeSized(bool themeSized)
{
	m_themeSized = themeSized;
	applySize();
}

/* Theme sized: every button is a square of the height of a row of the dock's
 * lists (what the theme's density makes of a control), the pan and tilt
 * block is three of them across and down, and the zoom pair fills the same
 * height. Otherwise, the buttons stretch, and are kept square. */
void PTZMovementControls::applySize()
{
	auto controls = PTZControls::getInstance();
	const QList<QToolButton *> buttons = findChildren<QToolButton *>();

	if (!m_themeSized) {
		ui->movementControlsGridLayout->setContentsMargins(m_margins);
		installEventFilter(m_squareFilter);
		ui->pantiltStack->installEventFilter(m_squareFilter);
		for (QToolButton *button : buttons) {
			button->setMinimumSize(0, 0);
			button->setMaximumSize(QWIDGETSIZE_MAX, QWIDGETSIZE_MAX);
		}
		ui->pantiltStack->setMinimumSize(0, 0);
		ui->pantiltStack->setMaximumSize(QWIDGETSIZE_MAX, QWIDGETSIZE_MAX);
		setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
		return;
	}

	/* No margin round the buttons either, so that what is beside them, the
	 * video, can be as tall as they are and line up with them */
	ui->movementControlsGridLayout->setContentsMargins(0, 0, 0, 0);
	removeEventFilter(m_squareFilter);
	ui->pantiltStack->removeEventFilter(m_squareFilter);
	setMinimumHeight(0);
	ui->pantiltStack->setMinimumHeight(0);

	const int side = controls && controls->rowHeight() > 0 ? controls->rowHeight() : 30;
	const int gap = ui->pantiltGridLayout->spacing();
	const int block = 3 * side + 2 * gap;
	for (QToolButton *button : buttons)
		button->setFixedSize(side, side);
	ui->pantiltStack->setFixedSize(block, block);
	/* Two buttons and the gap between them, as tall as the block */
	for (QToolButton *zoom : {ui->zoomButton_tele, ui->zoomButton_wide})
		zoom->setFixedSize(side, (block - gap) / 2);
	setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Fixed);
	updateGeometry();
}

/* Stop a camera that is being moved, and forget how it was */
void PTZMovementControls::stop()
{
	accel_timer.stop();
	if (pantiltingFlag || zoomingFlag || focusingFlag)
		ptzDeviceList->callDevice(device(), "ptz_stop");
	pantiltingFlag = false;
	zoomingFlag = false;
	focusingFlag = false;
	pan_speed = pan_accel = 0.0;
	tilt_speed = tilt_accel = 0.0;
	zoom_speed = zoom_accel = 0.0;
	focus_speed = focus_accel = 0.0;
}

void PTZMovementControls::setDevice(const QModelIndex &index)
{
	stop();
	m_device = index;
	updateControls();
}

/* Focusing by hand is only for when autofocus is off */
void PTZMovementControls::updateFocusControls()
{
	auto index = device();
	bool manual = !ui->focusButton_auto->isChecked();
	bool free = !m_locked;
	ui->focusButton_auto->setEnabled(free && PTZListModel::hasFeature(index, "autofocus"));
	ui->focusButton_near->setEnabled(free && manual && PTZListModel::hasFeature(index, "focus"));
	ui->focusButton_far->setEnabled(free && manual && PTZListModel::hasFeature(index, "focus"));
	ui->focusButton_onetouch->setEnabled(free && manual && PTZListModel::hasFeature(index, "focus_onetouch"));
}

void PTZMovementControls::updateControls()
{
	auto index = device();
	auto controls = PTZControls::getInstance();
	bool is_locked = controls && controls->liveMoveLockActive() && index.data(PTZListModel::IsLockedRole).toBool();
	/* A locked camera's controls are disabled one by one, not as a whole:
	 * a disabled widget has no context menu, and the ones on the pan/tilt
	 * area still have to show. */
	m_locked = is_locked;

	/* Only what the camera can do, and only when it isn't locked */
	bool pantilt = !is_locked && PTZListModel::hasFeature(index, "pantilt");
	const QList<QWidget *> pantiltControls = {
		ui->panTiltButton_upleft, ui->panTiltButton_up,        ui->panTiltButton_upright,
		ui->panTiltButton_left,   ui->panTiltButton_right,     ui->panTiltButton_downleft,
		ui->panTiltButton_down,   ui->panTiltButton_downright, ui->panTiltTouch,
	};
	for (QWidget *control : pantiltControls)
		control->setEnabled(pantilt);
	ui->panTiltButton_home->setEnabled(!is_locked && PTZListModel::hasFeature(index, "home"));
	ui->zoomButton_tele->setEnabled(!is_locked && PTZListModel::hasFeature(index, "zoom"));
	ui->zoomButton_wide->setEnabled(!is_locked && PTZListModel::hasFeature(index, "zoom"));

	OBSDataAutoRelease state = obs_data_create();
	ptzDeviceList->saveState(index, state.Get());
	showAutofocus(obs_data_get_bool(state, "focus_af_enabled"));
}

bool PTZMovementControls::onscreenJoystick() const
{
	return ui->pantiltStack->currentIndex() != 0;
}

void PTZMovementControls::setOnscreenJoystick(bool enabled)
{
	ui->pantiltStack->setCurrentIndex(enabled ? 1 : 0);
}

/**
 * callDevice - Helpers for sending device commands
 *
 * The first accepts a calldata structure with all the command's arguments;
 * the other is for the common case of one number. They use a fixed-stack
 * calldata, which is faster because it puts all the data onto the stack and
 * doesn't call bzalloc()/bfree() in the calldata code.
 */
bool PTZMovementControls::callDevice(const char *method, calldata_t *cd) const
{
	return ptzDeviceList->callDevice(device(), method, cd);
}

bool PTZMovementControls::callDevice(const char *method, const char *arg, double val) const
{
	calldata cd;
	uint8_t stack[128];
	calldata_init_fixed(&cd, stack, sizeof(stack));
	calldata_set_float(&cd, arg, val);
	return callDevice(method, &cd);
}

void PTZMovementControls::accelTimerHandler()
{
	calldata cd;
	uint8_t stack[128];
	calldata_init_fixed(&cd, stack, sizeof(stack));

	if (!device().isValid()) {
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

	callDevice("ptz_move", &cd);

	if (pan_accel == 0.0 && tilt_accel == 0.0 && zoom_accel == 0.0 && focus_accel == 0.0)
		accel_timer.stop();
}

void PTZMovementControls::setPanTilt(double pan, double tilt, double pan_accel_, double tilt_accel_)
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
	callDevice("ptz_move", &cd);
	calldata_free(&cd);
}

void PTZMovementControls::keypressPanTilt(double pan, double tilt)
{
	auto modifiers = QGuiApplication::keyboardModifiers();
	double speed = 0.5;
	double ramp = 0;

	if (modifiers.testFlag(Qt::ControlModifier))
		speed = 1.0;
	else if (modifiers.testFlag(Qt::ShiftModifier))
		speed = 0.05;
	else if (PTZControls::getInstance() && PTZControls::getInstance()->speedRampEnabled())
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
void PTZMovementControls::setZoom(double zoom)
{
	auto modifiers = QGuiApplication::keyboardModifiers();
	double speed = 0.5;
	zoomingFlag = (zoom != 0.0);
	if (modifiers.testFlag(Qt::ControlModifier))
		speed = 1.0;
	else if (modifiers.testFlag(Qt::ShiftModifier))
		speed = 0.1;

	callDevice("ptz_move", "zoom", zoom * speed);
}

void PTZMovementControls::setFocus(double focus)
{
	auto modifiers = QGuiApplication::keyboardModifiers();
	double speed = 0.5;
	focusingFlag = (focus != 0.0);
	if (modifiers.testFlag(Qt::ControlModifier))
		speed = 1.0;
	else if (modifiers.testFlag(Qt::ShiftModifier))
		speed = 0.1;

	callDevice("ptz_move", "focus", focus * speed);
}

/* The pan/tilt buttons are a large block of simple and mostly identical code.
 * Use C preprocessor macro to create all the duplicate functions */
#define button_pantilt_actions(direction, x, y)                     \
	void PTZMovementControls::on_panTiltButton_##direction##_pressed()  \
	{                                                           \
		keypressPanTilt(x, y);                              \
	}                                                           \
	void PTZMovementControls::on_panTiltButton_##direction##_released() \
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

void PTZMovementControls::on_panTiltButton_home_released()
{
	callDevice("ptz_home_recall");
}

/* There are fewer buttons for zoom or focus; so don't bother with macros */
void PTZMovementControls::on_zoomButton_tele_pressed()
{
	setZoom(1);
}

void PTZMovementControls::on_zoomButton_tele_released()
{
	setZoom(0);
}

void PTZMovementControls::on_zoomButton_wide_pressed()
{
	setZoom(-1);
}

void PTZMovementControls::on_zoomButton_wide_released()
{
	setZoom(0);
}

void PTZMovementControls::on_focusButton_auto_clicked(bool checked)
{
	requestAutofocus(checked);
}

void PTZMovementControls::on_focusButton_near_pressed()
{
	setFocus(1);
}

void PTZMovementControls::on_focusButton_near_released()
{
	setFocus(0);
}

void PTZMovementControls::on_focusButton_far_pressed()
{
	setFocus(-1);
}

void PTZMovementControls::on_focusButton_far_released()
{
	setFocus(0);
}

void PTZMovementControls::on_focusButton_onetouch_clicked()
{
	calldata cd = {};
	calldata_set_string(&cd, "name", "focus_onetouch");
	callDevice("ptz_trigger", &cd);
	calldata_free(&cd);
}

/* Follow the camera's autofocus: the button shows it, and what is left to
 * focus by hand is only for when it is off */
void PTZMovementControls::showAutofocus(bool on)
{
	ui->focusButton_auto->setChecked(on);
	updateFocusControls();
}

/* Ask the camera to turn its autofocus on or off */
void PTZMovementControls::requestAutofocus(bool on)
{
	showAutofocus(on);
	OBSDataAutoRelease request = obs_data_create();
	obs_data_set_bool(request, "focus_af_enabled", on);
	ptzDeviceList->setState(device(), request.Get());
}

bool PTZMovementControls::autofocusOn() const
{
	return ui->focusButton_auto->isChecked();
}

void PTZMovementControls::addContextActions(QMenu *menu, const QPoint &pos)
{
	/* Right-click on the Home button -> "Save current position as Home"
	 * (only shown for a device whose features have "home_set"). Single-click
	 * still triggers GotoHome; the context menu is purely additive. */
	if (childAt(pos) == ui->panTiltButton_home && PTZListModel::hasFeature(device(), "home_set")) {
		QAction *setHome = menu->addAction(obs_module_text("PTZ.Action.SetHome"));
		setHome->setEnabled(!m_locked);
		connect(setHome, &QAction::triggered, this, [this] { callDevice("ptz_home_save"); });
		menu->addSeparator();
	}

	QAction *touchpad = menu->addAction(obs_module_text("PTZ.Dock.OnscreenJoystick"));
	touchpad->setCheckable(true);
	touchpad->setChecked(onscreenJoystick());
	connect(touchpad, &QAction::toggled, this, &PTZMovementControls::setOnscreenJoystick);
}

void PTZMovementControls::contextMenuEvent(QContextMenuEvent *event)
{
	/* Left to whatever it is in, to show a menu of its own */
	if (!m_ownContextMenu) {
		event->ignore();
		return;
	}
	QMenu menu(this);
	addContextActions(&menu, event->pos());
	menu.exec(event->globalPos());
	event->accept();
}
