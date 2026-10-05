/* Widget showing a PTZ device's transient state
 *
 * Copyright 2026 Grant Likely <grant.likely@secretlab.ca>
 *
 * SPDX-License-Identifier: GPLv2
 */
#pragma once

#include <memory>
#include <optional>
#include <vector>
#include <QWidget>
#include <QTimer>
#include <QVariantMap>
#include <obs.hpp>

class QCheckBox;
class QComboBox;
class QFormLayout;
class QGroupBox;
class QLabel;
class QPushButton;
class QVBoxLayout;

/* Shows what a device reports of itself (the "ptz_get_state" proc): whether it is
 * connected, live, in the preview and locked, and, for a camera
 * that reports them, its power, autofocus and tally
 * lamps' state, its white balance, and the rest of what a VISCA camera reports
 * of its focus, exposure, picture, system and pan/tilt, most of which can be
 * changed. For a device that has them ("diagnostics" in its "features"),
 * buttons for its diagnostics.
 *
 * Every widget is made once, and an update changes only the ones whose value
 * changed, so nothing is torn down and rebuilt as the state changes many
 * times a second, the way a properties view would. A row is shown while its
 * key is in the state and hidden while it isn't, so what is shown follows
 * what the device reports.
 *
 * What the user edits doesn't change the view: it is asked of the device
 * (stateRequested()), and the view follows once the device says it has
 * changed. */
class PTZStateView : public QWidget {
	Q_OBJECT

public:
	explicit PTZStateView(QWidget *parent = nullptr);
	~PTZStateView() override;

	/* Show all of a device's state, as PTZDevice::saveState() gives it. A
	 * key that isn't in it isn't shown, and the rest is reset. */
	void setState(OBSData state);
	/* Fold in only what changed, the diff a device's state_changed signal
	 * carries, leaving the rest as it was. */
	void applyChanges(OBSData changed);
	/* Show a device's statistics, as PTZDevice::saveStatistics() gives
	 * them: read from it now and then, not part of its state */
	void setStatistics(OBSData statistics);

	/* What is shown, key by key: the value of each row that is visible. For
	 * tests, and anything else that wants to know what the user sees. */
	QVariantMap shownValues() const;
	/* How many updates changed something that is shown */
	int updateCount() const { return m_updateCount; }

	/* The diagnostics buttons' group, which is shown only for a device that
	 * has diagnostics. It starts in this view; a caller that wants it
	 * elsewhere (a tab of its own, say) can add it to another layout, and
	 * follow diagnosticsAvailableChanged() to know when it has anything. */
	QGroupBox *diagnosticsGroup() const { return m_diagnosticsGroup; }

signals:
	/* Something that is shown changed (what updateCount() counts) */
	void shownChanged();
	/* The device gained or lost diagnostics */
	void diagnosticsAvailableChanged(bool available);
	/* The user asked for these values, as a state object with just them in it
	 * (see PTZDevice::requestState()) */
	void stateRequested(OBSData requested);
	/* The user asked for a one-shot action that isn't a state, by its
	 * ptz_trigger name ("wb_onepush", say) */
	void actionRequested(const QString &action);
	/* The user asked for a camera report (see PTZCameraReportDialog) */
	void cameraReportRequested();

private:
	static constexpr int AxisCount = 4;

	/* One state key's row, of the ones made from a table rather than by
	 * hand: see ptz-state-view.cpp. Shown as a checkbox, a list to pick
	 * from, a number, or text that can't be edited. */
	struct Field;
	enum FieldKind { FlagField, ChoiceField, NumberField, TextField };

	void applyData(obs_data_t *data, bool all);
	void showWhiteBalance(int mode);
	void setRowVisible(QWidget *label, QWidget *field, bool visible);

	QFormLayout *addGroup(QVBoxLayout *page, const char *text);
	Field *addField(QFormLayout *form, const char *key, FieldKind kind, const char *text);
	void requestField(Field *field, const QVariant &value);
	bool showField(Field *field, const QVariant &value);
	void showShutterSpeeds();

	QCheckBox *m_connected;
	QCheckBox *m_live;
	QCheckBox *m_preview;
	QCheckBox *m_locked;

	/* Unlike the indicators above, these are commandable (PTZDevice::
	 * requestState()): the user can click them, and the row hides while
	 * the device doesn't report the key at all. */
	QCheckBox *m_power;
	QCheckBox *m_focusAuto;
	QCheckBox *m_tally;
	QCheckBox *m_tallyPreview;

	/* Where the camera is. Not shown here: the settings dialog's status line
	 * shows it, from shownValues(), so it is only kept, to 3 places, and
	 * reported when it changes (shownChanged()). */
	const char *m_axisKeys[AxisCount] = {"pan", "tilt", "zoom", "focus"};
	std::optional<double> m_axis[AxisCount];

	QGroupBox *m_whiteBalanceGroup;
	QComboBox *m_whiteBalanceMode;
	QPushButton *m_onePush;
	/* The mode the device last reported, which the list goes back to if the
	 * device doesn't do what the user asked for */
	int m_reportedWhiteBalance = -1;
	QTimer m_whiteBalanceSettle;

	/* Buttons for a driver's diagnostics, for a device that has them */
	QGroupBox *m_diagnosticsGroup;
	/* The device's statistics, in a table of what it has counted and how
	 * fast: a row for each, with the statistic it shows a total from and a
	 * rate from, and the labels the two are shown in */
	struct StatisticRow {
		const char *name;
		const char *label;
		const char *total;
		const char *rate;
		const char *rateFormat;
		QLabel *totalLabel = nullptr;
		QLabel *rateLabel = nullptr;
	};
	std::vector<StatisticRow> m_statisticRows;
	/* ...and how long the camera takes to answer a poll */
	QLabel *m_pollCycle;

	std::vector<std::unique_ptr<Field>> m_fields;
	/* The group boxes the fields are in; each is shown while any of its
	 * rows is. The white balance one is one of them. */
	std::vector<QGroupBox *> m_groups;
	/* Shutter speeds are named differently at 50 Hz */
	bool m_50Hz = false;

	int m_updateCount = 0;
};
