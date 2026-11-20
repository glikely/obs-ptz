/* Pan Tilt Zoom Controls - a source's video at the full frame rate
 *
 * Copyright 2026 Grant Likely <grant.likely@secretlab.ca>
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#pragma once

#include <mutex>
#include <obs.hpp>
#include <QImage>
#include <QList>
#include <QPersistentModelIndex>
#include <QPointer>
#include <QString>
#include <OBSQTDisplay.hpp>

/* Shows a device's source's video as OBS draws it, every frame, scaled to fit: an
 * OBSQTDisplay (OBS Studio's own display widget, vendored in
 * shared/qt/display) with a draw callback that renders the source into it.
 * OBS's graphics thread draws the source into the widget's window, which is
 * as smooth as OBS's own previews, and costs nothing while the widget is
 * hidden. */
class PTZSourceDisplay : public OBSQTDisplay {
	Q_OBJECT

	/* For whatever reads widgets by their Qt properties, as tests do */
	Q_PROPERTY(QString shownSource READ shownSource)
	Q_PROPERTY(QString overlayText READ overlayText)

public:
	explicit PTZSourceDisplay(QWidget *parent = nullptr);
	~PTZSourceDisplay() override;

	/* Follow this device, as the list model has it, showing its source with
	 * its name and status over it */
	void setDevice(const QModelIndex &index);

	/* The name of the source being shown, empty if there is none */
	QString shownSource() const;
	QString overlayText() const { return m_overlayText; }

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

	QPersistentModelIndex m_device;
	QString m_overlayText;

	/* Read by the graphics thread's draw callback as well as set here */
	mutable std::mutex m_lock;
	OBSWeakSource m_source;
	/* m_source has been told it is showing, to tell it it isn't. A flag, not a
	 * reference: this must not keep a camera's source, and so its device,
	 * alive after it is removed. */
	bool m_showing = false;
	/* What to draw over the video. draw() can't ask the model, which is the
	 * main thread's, so the text is painted there and handed over like this. */
	QImage m_overlayImage;
	bool m_overlayChanged = false;
	/* The graphics thread's, from m_overlayImage */
	gs_texture_t *m_overlayTexture = nullptr;

	void updateOverlay();
	QImage paintOverlay(const QString &text) const;
	/* The source to show, or none. Holds it weakly, but keeps it showing
	 * (obs_source_inc_showing()) while this can be seen, so that it renders
	 * even when it is in no scene on screen. */
	void setSource(OBSSource source);
	void holdShowingLocked();
	void releaseShowing();
	void releaseShowingLocked();
	static void draw(void *param, uint32_t cx, uint32_t cy);
};
