/* Pan Tilt Zoom Controls - a source's video at the full frame rate
 *
 * Copyright 2026 Grant Likely <grant.likely@secretlab.ca>
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#pragma once

#include <mutex>
#include <obs.hpp>
#include <QList>
#include <QPointer>
#include <OBSQTDisplay.hpp>

/* Shows a source's video as OBS draws it, every frame, scaled to fit: an
 * OBSQTDisplay (OBS Studio's own display widget, vendored in
 * shared/qt/display) with a draw callback that renders the source into it.
 * OBS's graphics thread draws the source into the widget's window, which is
 * as smooth as OBS's own previews, and costs nothing while the widget is
 * hidden. */
class PTZSourceDisplay : public OBSQTDisplay {
	Q_OBJECT

public:
	explicit PTZSourceDisplay(QWidget *parent = nullptr);
	~PTZSourceDisplay() override;

	/* The source to show, or none. Holds it weakly, but keeps it showing
	 * (obs_source_inc_showing()) so that it renders even when it is in no
	 * scene on screen. */
	void setSource(OBSSource source);
	OBSSource source() const;

	QSize sizeHint() const override;
	QSize minimumSizeHint() const override;

protected:
	void showEvent(QShowEvent *event) override;
	void hideEvent(QHideEvent *event) override;
	bool eventFilter(QObject *watched, QEvent *event) override;

private:
	/* The widgets this one is in, watched for them moving: see
	 * syncNativeGeometry() */
	QList<QPointer<QWidget>> m_watched;
	bool m_syncQueued = false;

	void watchAncestors();
	void unwatchAncestors();
	void syncNativeGeometry();

	/* Read by the graphics thread's draw callback as well as set here */
	mutable std::mutex m_lock;
	OBSWeakSource m_source;
	/* m_source has been told it is showing, to tell it it isn't. A flag, not a
	 * reference: this must not keep a camera's source, and so its device,
	 * alive after it is removed. */
	bool m_showing = false;

	void releaseShowing();
	void releaseShowingLocked();
	static void draw(void *param, uint32_t cx, uint32_t cy);
};
