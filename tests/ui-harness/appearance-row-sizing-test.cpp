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
	/* "presetListView": src/ptz-controls.ui. "sources": OBS's own
	 * frontend/forms/OBSBasic.ui. */
	logFirstRowHeight(mainWindow, "presetListView", "preset");
	logFirstRowHeight(mainWindow, "sources", "sources");
}

/* PTZControls::showEvent() (src/ptz-controls.cpp) pads ptzToolbar and
 * presetToolbar's minimum height to match OBS's own dock toolbars
 * (e.g. sourcesToolbar, on the Sources dock) - see that function's own
 * comment for why a brute-force pad is needed at all. Measuring
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

	auto measure = [mainWindow]() -> QList<int> {
		QList<int> state;
		auto *view = mainWindow ? mainWindow->findChild<QListView *>("presetListView") : nullptr;
		QModelIndex firstRow = view && view->model() ? view->model()->index(0, 0, view->rootIndex())
							     : QModelIndex();
		state << (firstRow.isValid() ? view->visualRect(firstRow).height() : -1);

		auto toolbarHeightOf = [mainWindow](const char *objectName) -> int {
			auto *tb = mainWindow ? mainWindow->findChild<QToolBar *>(QString::fromLatin1(objectName))
					      : nullptr;
			return tb ? tb->height() : -1;
		};
		state << toolbarHeightOf("ptzToolbar");
		state << toolbarHeightOf("presetToolbar");
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
	});

	settingsAction->trigger();
}

} // namespace

/* Registers the "appearance_row_sizing" test with harness: opens the
 * real Settings dialog, sets the requested Density/FontScale, clicks
 * the real Ok button, then logs this plugin's preset row height and
 * toolbar heights alongside the Sources dock's own row height and
 * toolbar height, for scripts/test_preset_row_sizing.py to compare.
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
}
