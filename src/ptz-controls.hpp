/* Pan Tilt Zoom camera controls
 *
 * Copyright 2020 Grant Likely <grant.likely@secretlab.ca>
 *
 * SPDX-License-Identifier: GPLv2
 */
#pragma once

#include "ptz.h"
#include <QTimer>
#include <QStyledItemDelegate>
#include <obs.hpp>
#include <obs-frontend-api.h>
#if defined(ENABLE_JOYSTICK)
#include <QJoysticks.h>
#endif
#include "touch-control.hpp"
#include "ui_ptz-controls.h"

typedef size_t ptz_joy_action_id;
enum ptz_joy_action {
	PTZ_JOY_ACTION_NONE = 0,
	PTZ_JOY_ACTION_PAN,
	PTZ_JOY_ACTION_PAN_INVERT,
	PTZ_JOY_ACTION_TILT,
	PTZ_JOY_ACTION_TILT_INVERT,
	PTZ_JOY_ACTION_ZOOM,
	PTZ_JOY_ACTION_ZOOM_INVERT,
	PTZ_JOY_ACTION_FOCUS,
	PTZ_JOY_ACTION_FOCUS_INVERT,
	PTZ_JOY_ACTION_LAST_VALUE
};
typedef enum ptz_joy_action ptz_joy_action_t;
extern const char *ptz_joy_action_axis_names[PTZ_JOY_ACTION_LAST_VALUE];

class PTZPresetListDelegate;
class PTZDeviceListDelegate;

class PTZControls : public QFrame {
	Q_OBJECT

private:
	static PTZControls *instance;
	static void onFrontendEvent(enum obs_frontend_event event, void *ptr);
	void handleFrontendEvent(enum obs_frontend_event event);
	static void onFrontendSaveEvent(obs_data_t *save_data, bool saving, void *ptr);
	/* The theme has changed (or the plugin has just started up);
	 * recalculate the row height and icon size shared by every list in
	 * this dock, to match the stock OBS theme. */
	void refreshTheme();
	int m_rowHeight = 0;
	int m_iconSize = 0;

	std::unique_ptr<Ui::PTZControls> ui;
	TouchControl *pantilt_widget;
	PTZPresetListDelegate *presetDelegate = nullptr;
	QSlider *presetZoomSlider = nullptr;
	QAction *presetZoomSpacer = nullptr;
	QAction *presetZoomAction = nullptr;
	QMetaObject::Connection presetSelectionConnection;
	PTZDeviceListDelegate *deviceDelegate = nullptr;

	bool live_move_lock_enabled = true;
	bool autoselect_enabled = false;
	bool speed_ramp_enabled = false;
	bool refresh_thumbnail_on_recall = true;

	void setPresetGridZoom(int percent);
	void holdCameraColumnWidth();
	void copyActionsDynamicProperties();
	void SaveConfig();
	void LoadConfig();

	void setCurrent(unsigned int index);
	int presetIndexToId(QModelIndex index);
	void presetSet(long long id);
	void presetRecall(long long id);
	void presetReset(long long id);

	bool callCurrentDevice(const char *method, calldata_t *cd = nullptr) const;
	bool callCurrentDevice(const char *method, const char *arg, long long val) const;

	QList<obs_hotkey_id> hotkeys;
	QMap<obs_hotkey_id, int> preset_hotkey_map;

public slots:
	void autoselectDevice(OBSSource scene);
private slots:
	void updateMoveControls();
	void currentChanged(QModelIndex current, QModelIndex previous);
	void settingsChanged(const QModelIndex &topleft, const QModelIndex &bottomRight);

	void updatePresetList();
	void presetUpdateActions();
	void on_presetListView_activated(QModelIndex index);
	void showContextMenu(const QPoint &pos);
	void on_actionProperties_triggered();
	void on_actionPresetAdd_triggered();
	void on_actionPresetRemove_triggered();
	void on_actionPresetMoveUp_triggered();
	void on_actionPresetMoveDown_triggered();
	void on_actionPresetSave_triggered();
	void on_actionPresetClear_triggered();
	void on_actionPresetRename_triggered();
	void on_actionPresetExport_triggered(QString filename = "");
	void on_actionPresetImport_triggered(QString filename = "");
	void on_actionPresetGridView_toggled(bool checked);

	/* Joystick support */
protected:
	bool eventFilter(QObject *watched, QEvent *event) override;
	bool m_joystick_enable = false;
	int m_joystick_id = -1;
	double m_joystick_deadzone = 0.0;
	double m_joystick_speed = 1.0;
	int joystick_pan_axis = -1;
	bool joystick_pan_invert = false;
	int joystick_tilt_axis = -1;
	bool joystick_tilt_invert = false;
	int joystick_zoom_axis = -1;
	bool joystick_zoom_invert = false;
	int joystick_focus_axis = -1;
	bool joystick_focus_invert = false;
	QMap<size_t, ptz_joy_action_id> joystick_axis_actions;
	QMap<size_t, QString> joystick_button_hotkey_mappings;

public slots:
	void setJoystickAxisAction(size_t axis, ptz_joy_action_id);
	void setJoystickButtonHotkey(size_t button, QString);

signals:
	void joystickAxisActionChanged(size_t axis, ptz_joy_action_id action);
	void joystickButtonHotkeyChanged(size_t button, QString hotkey);

#if defined(ENABLE_JOYSTICK)
public:
	void joystickSetup();
	bool joystickEnabled() { return m_joystick_enable; };
	double joystickDeadzone() { return m_joystick_deadzone; };
	double joystickSpeed() { return m_joystick_speed; };
	void setJoystickEnabled(bool enable);
	void setJoystickSpeed(double speed);
	void setJoystickDeadzone(double deadzone);
	int joystickId() { return m_joystick_id; };
	void setJoystickId(int id) { m_joystick_id = id; };
	double readAxis(const QJoystickDevice *jd, int axis, bool invert);
	ptz_joy_action_id joystickAxisAction(size_t axis) { return joystick_axis_actions[axis]; };
	QString joystickButtonHotkey(size_t button) { return joystick_button_hotkey_mappings[button]; };

protected slots:
	void joystickAxesChanged(const QJoystickDevice *jd, uint32_t updated);
	void joystickAxisEvent(const QJoystickAxisEvent evt);
	void joystickButtonEvent(const QJoystickButtonEvent evt);
	void joystickPOVEvent(const QJoystickPOVEvent evt);
#else
public:
	void joystickSetup() {};
#endif /* ENABLE_JOYSTICK */

public:
	PTZControls(QWidget *parent = nullptr);
	bool autoselectEnabled() { return autoselect_enabled; };
	bool liveMoveLockEnabled() { return live_move_lock_enabled; };
	bool liveMoveLockActive() { return live_move_lock_enabled && obs_frontend_preview_program_mode_active(); };
	bool speedRampEnabled() { return speed_ramp_enabled; };
	bool refreshThumbnailOnRecall() const { return refresh_thumbnail_on_recall; };
	static PTZControls *getInstance() { return instance; };
	int rowHeight() const { return m_rowHeight; }
	int iconSize() const { return m_iconSize; }

public slots:
	void setAutoselectEnabled(bool enable);
	void setLiveMoveLockEnabled(bool enable);
	void setSpeedRampEnabled(bool enable);
	void setRefreshThumbnailOnRecall(bool enable);

signals:
	/* The movement controls may need enabling differently: the selected
	 * device, or what locks it, changed. The dock's own controls and any
	 * other PTZMovementControls (the settings dialog's) listen. */
	void moveControlsChanged();
	/* The theme has changed, and the icons that follow it should too */
	void themeRefreshed();
	void autoselectEnabledChanged(bool enabled);
	void liveMoveLockEnabledChanged(bool enabled);
	void speedRampEnabledChanged(bool enabled);
	void refreshThumbnailOnRecallChanged(bool enabled);
};

class PTZDeviceListDelegate : public QStyledItemDelegate {
	Q_OBJECT
	Q_PROPERTY(int iconSize READ iconSize)

public:
	struct CellLayout {
		int iconMargin;
		int tallyMargin;
		QRect text;
		QRect status;
		QRect lock;
		QRect tally;
	};

	PTZDeviceListDelegate(QObject *parent);
	virtual QSize sizeHint(const QStyleOptionViewItem &option, const QModelIndex &index) const override;
	virtual void paint(QPainter *painter, const QStyleOptionViewItem &option,
			   const QModelIndex &index) const override;
	virtual bool editorEvent(QEvent *event, QAbstractItemModel *model, const QStyleOptionViewItem &option,
				 const QModelIndex &index) override;
	virtual bool helpEvent(QHelpEvent *event, QAbstractItemView *view, const QStyleOptionViewItem &option,
			       const QModelIndex &index) override;

	int iconSize() const { return PTZControls::getInstance()->iconSize(); }
	void refreshTheme();

private:
	CellLayout layoutCell(const QModelIndex &index, const QStyleOptionViewItem &option) const;

	QIcon lockedIcon;
	QIcon unlockedIcon;
	QIcon disconnectedIcon;
	QIcon poweredOffIcon;
	mutable int m_iconSize = 0;
};

class PTZPresetListDelegate : public QStyledItemDelegate {
	Q_OBJECT
	Q_PROPERTY(int iconSize READ iconSize)

public:
	struct CellLayout {
		int iconMargin;
		QRect thumbnail;
		QRect text;
		QRect recall;
	};

	PTZPresetListDelegate(QObject *parent);
	virtual QSize sizeHint(const QStyleOptionViewItem &option, const QModelIndex &index) const override;
	virtual void paint(QPainter *painter, const QStyleOptionViewItem &option,
			   const QModelIndex &index) const override;
	virtual bool editorEvent(QEvent *event, QAbstractItemModel *model, const QStyleOptionViewItem &option,
				 const QModelIndex &index) override;
	virtual bool helpEvent(QHelpEvent *event, QAbstractItemView *view, const QStyleOptionViewItem &option,
			       const QModelIndex &index) override;

	int iconSize() const { return PTZControls::getInstance()->iconSize(); }
	void refreshTheme();
	/* Show the presets as a grid of thumbnails, rather than as a list */
	void setGridMode(bool grid);
	bool gridMode() const { return m_gridMode; }
	/* The gap the grid leaves around each cell */
	static constexpr int gridSpacing = 2;
	/* The zoom sets how wide a thumbnail should be, as a percent of its
	 * default. That decides how many columns fit across the view; the
	 * columns are then widened to take up the space left over. */
	static constexpr int minGridZoom = 50;
	static constexpr int maxGridZoom = 200;
	void setGridZoom(int percent);
	int gridZoom() const { return m_gridZoom; }
	/* Whether the width the view lays its cells out in has changed since
	 * this was last asked, which means the cell sizes need working out again */
	bool layoutWidthChanged(const QAbstractScrollArea *view);

private:
	CellLayout layoutCell(const QModelIndex &index, const QStyleOptionViewItem &option) const;
	int gridCellWidth(const QWidget *view) const;
	static int layoutWidth(const QAbstractScrollArea *view);

	QIcon recallIcon;
	bool m_gridMode = false;
	int m_gridZoom = 100;
	int m_lastLayoutWidth = 0;
};
