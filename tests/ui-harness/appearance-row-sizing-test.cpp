/* PTZ UI test harness: Appearance row-sizing test
 *
 * Copyright 2026 Grant Likely <grant.likely@secretlab.ca>
 *
 * SPDX-License-Identifier: GPLv2
 */
#include "ui-test-harness.hpp"

#include <obs-module.h>
#include <obs-frontend-api.h>

#include <QWidget>
#include <QListView>
#include <QAbstractItemDelegate>
#include <QToolBar>
#include <QAction>
#include <QDialog>
#include <QButtonGroup>
#include <QAbstractButton>
#include <QPushButton>
#include <QSlider>
#include <QDialogButtonBox>
#include <QTimer>
#include <QEventLoop>
#include <QElapsedTimer>
#include <QCheckBox>
#include <QVariant>
#include <QStyle>

namespace {

/* Measures a QAbstractItemView the same generic way regardless of
 * which dock it belongs to - this plugin's preset list or OBS's own
 * Sources dock - so this test never needs to reach into
 * PTZPresetListDelegate's internals (or OBS's SourceTreeItem's).
 *
 * Resolves the first row under view->rootIndex() rather than
 * view->model()->index(0, 0): the Sources dock's "sources" is flat, so
 * rootIndex() is the default invalid index and the two are the same,
 * but this plugin's own "presetListView" is rooted at the first PTZ
 * device (ui->presetListView->setRootIndex(...) in PTZControls's
 * constructor) - index(0, 0) with no parent resolves to that *device*,
 * which isn't a row this view renders at all, not the first preset
 * under it. */
void logFirstRowHeight(QWidget *mainWindow, const char *objectName, const char *label)
{
	auto *view = mainWindow ? mainWindow->findChild<QListView *>(QString::fromLatin1(objectName)) : nullptr;
	QModelIndex firstRow = view && view->model() ? view->model()->index(0, 0, view->rootIndex()) : QModelIndex();
	if (!firstRow.isValid()) {
		blog(LOG_INFO, "[ptz-ui-test] %s: view/model/rows not available (need >=1 row)", label);
		return;
	}
	int height = view->visualRect(firstRow).height();
	blog(LOG_INFO, "[ptz-ui-test] %s rowHeight=%d", label, height);
}

void logRowHeights()
{
	auto *mainWindow = static_cast<QWidget *>(obs_frontend_get_main_window());
	/* "presetListView"/"deviceList": src/ptz-controls.ui. "sources":
	 * OBS's own frontend/forms/OBSBasic.ui. */
	logFirstRowHeight(mainWindow, "presetListView", "preset");
	logFirstRowHeight(mainWindow, "deviceList", "camera");
	logFirstRowHeight(mainWindow, "sources", "sources");
}

/* PTZControls::refreshToolbarSizes() (src/ptz-controls.cpp) mirrors
 * ptzToolbar/presetToolbar's minimum height directly from OBS's own
 * dock toolbars (e.g. sourcesToolbar, on the Sources dock) - see that
 * function's own comment for why matching it directly, rather than
 * trusting either toolbar's own sizeHint(), is the point. Measuring
 * ->height() rather than ->sizeHint() checks what's actually on
 * screen, not just what the widget would prefer to be. */
void logToolbarHeight(QWidget *mainWindow, const char *objectName, const char *label)
{
	auto *toolbar = mainWindow ? mainWindow->findChild<QToolBar *>(QString::fromLatin1(objectName)) : nullptr;
	if (!toolbar) {
		blog(LOG_INFO, "[ptz-ui-test] %s: toolbar not available", label);
		return;
	}
	blog(LOG_INFO, "[ptz-ui-test] %s toolbarHeight=%d", label, toolbar->height());
}

void logToolbarHeights()
{
	auto *mainWindow = static_cast<QWidget *>(obs_frontend_get_main_window());
	/* "ptzToolbar"/"presetToolbar": src/ptz-controls.ui.
	 * "sourcesToolbar": OBS's own frontend/forms/OBSBasic.ui. */
	logToolbarHeight(mainWindow, "ptzToolbar", "ptzToolbar");
	logToolbarHeight(mainWindow, "presetToolbar", "presetToolbar");
	logToolbarHeight(mainWindow, "sourcesToolbar", "sourcesToolbar");
}

/* The preset list's recall icon (PTZPresetListDelegate::iconSize,
 * src/ptz-controls.cpp) is meant to track the same real-world thing as
 * the Sources dock's own row-level icons: its vis/lock checkboxes
 * (styled ".checkbox-icon" in the theme) - not the fixed 16x16
 * scene/source-type QLabel icon, which never changes size at all.
 * Read from the delegate via QObject::property("iconSize")
 * (PTZPresetListDelegate declares it as a real Q_PROPERTY) rather than
 * a test-only accessor, so nothing in core code exists solely for this
 * test to call.
 *
 * Measures the checkbox's actual drawn glyph - its ::indicator
 * sub-control, via QStyle::PM_IndicatorHeight - not checkbox->height()
 * (the checkbox *widget*'s own on-screen bounding box). The two look
 * like they should be the same thing but aren't: the widget's box
 * grows past the glyph's own size once FontScale pushes the row itself
 * taller (QSizePolicy::Preferred lets the widget stretch to fill the
 * extra row height, same as the checkboxes described in
 * SourceTreeItem.cpp), but the glyph painted inside it - what a user
 * actually sees as "the icon" - does not grow at all; it stays exactly
 * OBS's own --icon_base value (max(2, obsPadding) + 12) at every
 * FontScale. Comparing against checkbox->height() here previously
 * masked a real bug: PTZPresetListDelegate::refreshTheme() had been
 * fitted to grow the recall icon with FontScale to match that widget
 * height, which was growing for a reason - the widget's own layout
 * stretch - that has nothing to do with icon size. */
void logDelegateIconSize(QWidget *mainWindow, const char *viewName, const char *label)
{
	auto *view = mainWindow ? mainWindow->findChild<QListView *>(QString::fromLatin1(viewName)) : nullptr;
	auto *delegate = view ? view->itemDelegate() : nullptr;
	QVariant iconSize = delegate ? delegate->property("iconSize") : QVariant();
	if (iconSize.isValid())
		blog(LOG_INFO, "[ptz-ui-test] %s recallIconSize=%d", label, iconSize.toInt());
	else
		blog(LOG_INFO, "[ptz-ui-test] %s: delegate/iconSize property not available", label);
}

void logIconSizes()
{
	auto *mainWindow = static_cast<QWidget *>(obs_frontend_get_main_window());

	/* "recallIconSize" in the log field name is a bit of a misnomer for
	 * deviceList (its icon is a lock/status glyph, not a recall
	 * button) - kept anyway so both share the exact same field name,
	 * and scripts/test_preset_row_sizing.py's parsing only needs one
	 * regex shape, parameterised by the leading label ("preset"/
	 * "camera"). */
	logDelegateIconSize(mainWindow, "presetListView", "preset");
	logDelegateIconSize(mainWindow, "deviceList", "camera");

	auto *sourcesView = mainWindow ? mainWindow->findChild<QListView *>("sources") : nullptr;
	QModelIndex firstRow = sourcesView && sourcesView->model()
				       ? sourcesView->model()->index(0, 0, sourcesView->rootIndex())
				       : QModelIndex();
	auto *item = firstRow.isValid() ? sourcesView->indexWidget(firstRow) : nullptr;
	auto *checkbox = item ? item->findChild<QCheckBox *>() : nullptr;
	if (checkbox) {
		int indicatorHeight = checkbox->style()->pixelMetric(QStyle::PM_IndicatorHeight, nullptr, checkbox);
		blog(LOG_INFO, "[ptz-ui-test] sources checkboxIconSize=%d", indicatorHeight);
	} else {
		blog(LOG_INFO, "[ptz-ui-test] sources: checkbox-icon not available");
	}
}

/* Waits for the theme-change reflow this test measures to actually
 * finish, instead of guessing a fixed delay: okButton->click() below
 * runs SetTheme() synchronously, but PTZPresetListDelegate's and
 * PTZControls's own response to that is deferred
 * (QTimer::singleShot(0, ...), see ptz-controls.cpp), and the
 * delegate's row-size change reaches the view through a further
 * Qt::QueuedConnection on top of that
 * (QAbstractItemDelegate::sizeHintChanged() -> doItemsLayout()).
 * Rather than model that whole chain by hand (or guess how long it
 * takes), poll the values this test cares about at a short interval
 * and treat the reflow as done once they read identically three times
 * in a row. Fast on the common case - most combinations settle within
 * a few polls, well under what a fixed delay long enough for the slow
 * case would cost every time. The one combination known to
 * occasionally take close to a second of real event-loop time (a
 * Density switch, which restyles the whole application) just keeps
 * polling rather than racing a fixed timeout, capped by a generous
 * ceiling as a safety net, not the primary mechanism. */
void waitForReflow(QWidget *mainWindow)
{
	constexpr int kPollMs = 15;
	constexpr int kRequiredStableSamples = 3;
	constexpr int kMaxWaitMs = 5000;

	auto rowHeightOf = [mainWindow](const char *objectName) -> int {
		auto *view = mainWindow ? mainWindow->findChild<QListView *>(QString::fromLatin1(objectName)) : nullptr;
		QModelIndex firstRow = view && view->model() ? view->model()->index(0, 0, view->rootIndex())
							     : QModelIndex();
		return firstRow.isValid() ? view->visualRect(firstRow).height() : -1;
	};
	auto iconSizeOf = [mainWindow](const char *objectName) -> int {
		auto *view = mainWindow ? mainWindow->findChild<QListView *>(QString::fromLatin1(objectName)) : nullptr;
		auto *delegate = view ? view->itemDelegate() : nullptr;
		return delegate ? delegate->property("iconSize").toInt() : -1;
	};
	auto measure = [=]() -> QList<int> {
		QList<int> state;
		state << rowHeightOf("presetListView");
		state << rowHeightOf("deviceList");

		auto toolbarHeightOf = [mainWindow](const char *objectName) -> int {
			auto *tb = mainWindow ? mainWindow->findChild<QToolBar *>(QString::fromLatin1(objectName))
					      : nullptr;
			return tb ? tb->height() : -1;
		};
		state << toolbarHeightOf("ptzToolbar");
		state << toolbarHeightOf("presetToolbar");

		state << iconSizeOf("presetListView");
		state << iconSizeOf("deviceList");
		return state;
	};

	QElapsedTimer overall;
	overall.start();
	QList<int> prev = measure();
	int stableSamples = 0;

	while (overall.elapsed() < kMaxWaitMs) {
		QEventLoop loop;
		QTimer::singleShot(kPollMs, &loop, &QEventLoop::quit);
		loop.exec();

		QList<int> cur = measure();
		if (cur == prev) {
			if (++stableSamples >= kRequiredStableSamples)
				return;
		} else {
			stableSamples = 0;
		}
		prev = cur;
	}
	blog(LOG_INFO, "[ptz-ui-test] waitForReflow: gave up after %dms without the measured values settling",
	     kMaxWaitMs);
}

void runAppearanceRowSizingTest(const QMap<QString, QString> &params)
{
	bool densityOk = false, fontScaleOk = false;
	int density = params.value(QStringLiteral("density")).toInt(&densityOk);
	int fontScale = params.value(QStringLiteral("fontscale")).toInt(&fontScaleOk);
	if (!densityOk || !fontScaleOk) {
		blog(LOG_INFO, "[ptz-ui-test] appearance_row_sizing: malformed density/fontscale, ignoring");
		return;
	}
	blog(LOG_INFO, "[ptz-ui-test] appearance_row_sizing: requesting density=%d fontscale=%d", density, fontScale);

	/* obs-frontend-api has no call to trigger OBSApp::SetTheme() (the
	 * thing that actually re-reads Appearance/FontScale+Density and
	 * regenerates the live stylesheet) - it's a private method on the
	 * internal OBSApp class, unreachable from any plugin through
	 * obs-frontend-api. But OBSBasicSettings - the real Settings
	 * dialog - calls it via the exact same public Qt widgets a user
	 * clicks, reachable by object name once the dialog is open, so
	 * drive it exactly as a user would instead: open Settings, set the
	 * density button and font slider, click Ok.
	 * OBSBasicSettings::exec() (frontend/widgets/
	 * OBSBasic_MainControls.cpp, on_action_Settings_triggered()) is
	 * modal, so the widget-finding and Ok-click below is scheduled to
	 * run *during* that nested event loop, before triggering the
	 * action that opens it. */
	auto *mainWindow = static_cast<QWidget *>(obs_frontend_get_main_window());
	auto *settingsAction = mainWindow ? mainWindow->findChild<QAction *>("action_Settings") : nullptr;
	if (!settingsAction) {
		blog(LOG_INFO, "[ptz-ui-test] action_Settings not found");
		return;
	}

	QTimer::singleShot(300, [mainWindow, density, fontScale]() {
		auto *dialog = mainWindow->findChild<QDialog *>("OBSBasicSettings");
		if (!dialog) {
			blog(LOG_INFO, "[ptz-ui-test] OBSBasicSettings dialog didn't open in time");
			return;
		}

		auto *densityGroup = dialog->findChild<QButtonGroup *>("appearanceDensityButtonGroup");
		QAbstractButton *densityButton = densityGroup ? densityGroup->button(density) : nullptr;
		if (densityButton)
			densityButton->setChecked(true);
		else
			blog(LOG_INFO, "[ptz-ui-test] density button id %d not found", density);

		auto *fontSlider = dialog->findChild<QSlider *>("appearanceFontScale");
		if (fontSlider)
			fontSlider->setValue(fontScale);
		else
			blog(LOG_INFO, "[ptz-ui-test] appearanceFontScale slider not found");

		/* SaveAppearanceSettings() only runs from
		 * on_buttonBox_clicked(), which needs the actual
		 * QAbstractButton that was clicked to look up its
		 * ButtonRole - dialog->accept() would close the dialog
		 * without saving anything, so click the real Ok button
		 * instead. */
		auto *buttonBox = dialog->findChild<QDialogButtonBox *>("buttonBox");
		QAbstractButton *okButton = buttonBox ? buttonBox->button(QDialogButtonBox::Ok) : nullptr;
		if (!okButton) {
			blog(LOG_INFO, "[ptz-ui-test] Settings dialog Ok button not found");
			return;
		}
		okButton->click();

		waitForReflow(mainWindow);
		logRowHeights();
		logToolbarHeights();
		logIconSizes();
	});

	settingsAction->trigger();
}

/* Diagnostic-only: logs the current row/toolbar/icon geometry exactly
 * as-is, with no Settings-dialog interaction at all - unlike
 * runAppearanceRowSizingTest() below, which always drives a real
 * Density/FontScale change first. Added to chase a startup-only bug:
 * ptzToolbar/presetToolbar render at the wrong height on OBS's very
 * first show, before any Settings > Appearance round trip, and the
 * appearance_row_sizing sweep can never see that state since it always
 * forces a settings change before measuring. */
void runMeasureNowTest(const QMap<QString, QString> &)
{
	logRowHeights();
	logToolbarHeights();
	logIconSizes();
}

} // namespace

/* Registers the "appearance_row_sizing" test with harness: opens the
 * real Settings dialog, sets the requested Density/FontScale, clicks
 * the real Ok button, then logs this plugin's preset row height,
 * toolbar heights, and recall icon size alongside the Sources dock's
 * own row height, toolbar height, and checkbox-icon size, for
 * scripts/test_preset_row_sizing.py to compare.
 *
 * Request params:
 *   density   - Settings > Appearance > Density's button group id
 *               (-2 Classic, -3 Compact, -4 Normal, -5 Comfortable)
 *   fontscale - Settings > Appearance > Font Size slider value
 *               (the real UI only allows 8-12)
 */
void registerAppearanceRowSizingTest(PTZUITestHarness *harness)
{
	harness->registerTest(QStringLiteral("appearance_row_sizing"), &runAppearanceRowSizingTest);
	harness->registerTest(QStringLiteral("measure_now"), &runMeasureNowTest);
}
