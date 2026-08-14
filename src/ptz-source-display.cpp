/* Pan Tilt Zoom Controls - a source's video at the full frame rate
 *
 * Copyright 2026 Grant Likely <grant.likely@secretlab.ca>
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include <obs-module.h>
#include <graphics/graphics.h>
#include <graphics/matrix4.h>
#include <QTimer>
#include <QWindow>

#include "ptz-source-display.hpp"
#include "display-helpers.hpp"

PTZSourceDisplay::PTZSourceDisplay(QWidget *parent) : OBSQTDisplay(parent)
{
	setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);

	/* The display is made once the window is on screen, and again if it
	 * moves to another screen, so each time it is made is when to say what
	 * draws into it */
	connect(this, &OBSQTDisplay::DisplayCreated, this,
		[this]() { obs_display_add_draw_callback(GetDisplay(), draw, this); });
}

PTZSourceDisplay::~PTZSourceDisplay()
{
	unwatchAncestors();
	/* The display waits for a draw in progress when it goes, and must go
	 * before what the draw callback reads, which is members of this class,
	 * does: OBSQTDisplay would only do it after they are gone. */
	DestroyDisplay();
	releaseShowing();
}

/* The display is a window of its own, which Qt keeps where the widget is
 * when the widget moves or is resized. It doesn't always when only a
 * widget it is in did, and not the widget itself, relative to its parent: a
 * splitter handing out less room moves the pane the display is in, and the
 * display's window was left behind, with the picture in the wrong place.
 * So when any of them moves or resizes, once the layout has settled, put the
 * window where the widget is. */
void PTZSourceDisplay::watchAncestors()
{
	unwatchAncestors();
	for (QWidget *widget = parentWidget(); widget; widget = widget->parentWidget()) {
		widget->installEventFilter(this);
		m_watched.append(widget);
	}
}

void PTZSourceDisplay::unwatchAncestors()
{
	for (const QPointer<QWidget> &widget : m_watched)
		if (widget)
			widget->removeEventFilter(this);
	m_watched.clear();
}

void PTZSourceDisplay::showEvent(QShowEvent *event)
{
	OBSQTDisplay::showEvent(event);
	watchAncestors();
	syncNativeGeometry();
}

void PTZSourceDisplay::hideEvent(QHideEvent *event)
{
	OBSQTDisplay::hideEvent(event);
	unwatchAncestors();
}

bool PTZSourceDisplay::eventFilter(QObject *watched, QEvent *event)
{
	switch (event->type()) {
	case QEvent::Move:
	case QEvent::Resize:
	case QEvent::LayoutRequest:
		if (!m_syncQueued) {
			m_syncQueued = true;
			QTimer::singleShot(0, this, [this]() {
				m_syncQueued = false;
				syncNativeGeometry();
			});
		}
		break;
	default:
		break;
	}
	return OBSQTDisplay::eventFilter(watched, event);
}

void PTZSourceDisplay::syncNativeGeometry()
{
	QWindow *window = windowHandle();
	QWidget *native = nativeParentWidget();
	if (!window || !native || !isVisible())
		return;

	const QRect wanted(mapTo(native, QPoint(0, 0)), size());
	if (window->geometry() == wanted)
		return;

	window->setGeometry(wanted);
}

void PTZSourceDisplay::releaseShowing()
{
	std::lock_guard<std::mutex> guard(m_lock);
	releaseShowingLocked();
}

/* Tell the source it isn't showing any more, if it was told it was and is
 * still there: one that is gone has nothing to be told. */
void PTZSourceDisplay::releaseShowingLocked()
{
	if (!m_showing)
		return;
	m_showing = false;
	OBSSourceAutoRelease source = obs_weak_source_get_source(m_source);
	if (source)
		obs_source_dec_showing(source);
}

void PTZSourceDisplay::setSource(OBSSource source)
{
	OBSWeakSource weak = OBSGetWeakRef(source);
	std::lock_guard<std::mutex> guard(m_lock);
	if (m_source.Get() == weak.Get())
		return;
	releaseShowingLocked();
	m_source = weak;
	if (source) {
		obs_source_inc_showing(source);
		m_showing = true;
	}
}

OBSSource PTZSourceDisplay::source() const
{
	std::lock_guard<std::mutex> guard(m_lock);
	return OBSGetStrongRef(m_source);
}

/* The shape of the widget is the layout's to choose, not the source's: the
 * video is scaled to fit it, with bars where it doesn't */
QSize PTZSourceDisplay::sizeHint() const
{
	return QSize(240, 135);
}

QSize PTZSourceDisplay::minimumSizeHint() const
{
	return QSize(120, 68);
}

/* Runs on the graphics thread, once a frame, with the display's own size */
void PTZSourceDisplay::draw(void *param, uint32_t cx, uint32_t cy)
{
	auto *self = static_cast<PTZSourceDisplay *>(param);
	OBSSourceAutoRelease source;
	{
		std::lock_guard<std::mutex> guard(self->m_lock);
		source = obs_weak_source_get_source(self->m_source);
	}
	if (!source || !cx || !cy)
		return;

	uint32_t sourceCX = obs_source_get_width(source);
	uint32_t sourceCY = obs_source_get_height(source);
	if (!sourceCX || !sourceCY)
		return;

	int x, y;
	float scale;
	GetScaleAndCenterPos(sourceCX, sourceCY, cx, cy, x, y, scale);
	int newCX = int(scale * float(sourceCX));
	int newCY = int(scale * float(sourceCY));

	gs_viewport_push();
	gs_projection_push();
	/* As OBS's own previews do, or the colors come out wrong */
	const bool previous = gs_set_linear_srgb(true);
	gs_ortho(0.0f, float(sourceCX), 0.0f, float(sourceCY), -100.0f, 100.0f);
	gs_set_viewport(x, y, newCX, newCY);
	obs_source_video_render(source);
	gs_set_linear_srgb(previous);
	gs_projection_pop();
	gs_viewport_pop();
}
