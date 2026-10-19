/* PTZ view control: a source's picture, that moves its camera
 *
 * SPDX-License-Identifier: GPLv2
 */

#include <algorithm>
#include <obs.hpp>
#include <QMouseEvent>
#include <QWheelEvent>
#include <QWindow>
#if !defined(_WIN32) && !defined(__APPLE__)
#include <obs-nix-platform.h>
#endif
#include "ptz-view-control.hpp"
#include <qt-wrappers.hpp>
#include "ptz-controls.hpp"
#include "ptz-list-model.hpp"

/* How often moves are sent, and how much one notch of the wheel zooms */
static constexpr int tickMs = 100;
static constexpr double zoomPerNotch = 0.1;
/* After this long without the wheel, zoom from where the camera says it is
 * rather than from where it was last sent */
static constexpr qint64 zoomSettleMs = 1000;

static bool toGSWindow(QWindow *window, gs_window &gswindow)
{
#ifdef _WIN32
	gswindow.hwnd = (void *)window->winId();
#elif defined(__APPLE__)
	gswindow.view = (id)window->winId();
#else
	/* Wayland needs Qt's private native interface; not supported here */
	if (obs_get_nix_platform() != OBS_NIX_PLATFORM_X11_EGL)
		return false;
	gswindow.id = window->winId();
	gswindow.display = obs_get_nix_platform_display();
#endif
	return true;
}

PTZViewControl::PTZViewControl(QWidget *parent) : QWidget(parent)
{
	setAttribute(Qt::WA_PaintOnScreen);
	setAttribute(Qt::WA_NativeWindow);
	setAttribute(Qt::WA_NoSystemBackground);
	/* Or the dock it is in becomes a native window too */
	setAttribute(Qt::WA_DontCreateNativeAncestors);
	connect(&m_timer, &QTimer::timeout, this, &PTZViewControl::tick);
	m_timer.start(tickMs);
}

PTZViewControl::~PTZViewControl()
{
	if (m_display) {
		obs_display_remove_draw_callback(m_display, drawCallback, this);
		obs_display_destroy(m_display);
		m_display = nullptr;
	}
}

/* Runs on the graphics thread */
void PTZViewControl::drawCallback(void *param, uint32_t cx, uint32_t cy)
{
	auto view = static_cast<PTZViewControl *>(param);
	OBSSourceAutoRelease source;
	{
		QMutexLocker locker(&view->m_sourceMutex);
		source = obs_weak_source_get_source(view->m_source);
	}
	if (!source)
		return;

	uint32_t sourceCx = obs_source_get_width(source);
	uint32_t sourceCy = obs_source_get_height(source);
	if (!sourceCx || !sourceCy)
		return;

	/* Fit the source in the display, keeping its aspect, centered */
	float scale = std::min((float)cx / sourceCx, (float)cy / sourceCy);
	int width = (int)(sourceCx * scale);
	int height = (int)(sourceCy * scale);

	gs_projection_push();
	gs_viewport_push();
	gs_set_viewport((cx - width) / 2, (cy - height) / 2, width, height);
	gs_ortho(0.0f, (float)sourceCx, 0.0f, (float)sourceCy, -100.0f, 100.0f);
	obs_source_video_render(source);
	gs_viewport_pop();
	gs_projection_pop();
}

void PTZViewControl::createDisplay()
{
	if (m_display)
		return;
	winId();
	QWindow *window = windowHandle();
	if (!window)
		return;

	QSize size = this->size() * devicePixelRatioF();
	gs_init_data info = {};
	info.cx = size.width();
	info.cy = size.height();
	info.format = GS_BGRA;
	info.zsformat = GS_ZS_NONE;
	if (!toGSWindow(window, info.window))
		return;

	m_display = obs_display_create(&info, 0);
	if (m_display)
		obs_display_add_draw_callback(m_display, drawCallback, this);
}

QRectF PTZViewControl::sourceRect() const
{
	OBSSourceAutoRelease source;
	{
		QMutexLocker locker(&m_sourceMutex);
		source = obs_weak_source_get_source(m_source);
	}
	if (!source)
		return QRectF();
	double sourceCx = obs_source_get_width(source);
	double sourceCy = obs_source_get_height(source);
	if (!sourceCx || !sourceCy)
		return QRectF();
	double scale = std::min(width() / sourceCx, height() / sourceCy);
	QSizeF size(sourceCx * scale, sourceCy * scale);
	return QRectF(QPointF((width() - size.width()) / 2, (height() - size.height()) / 2), size);
}

/* A device is named after its source. Looked up each time, so that one made
 * after setSource(), or a renamed source, is still found. */
QModelIndex PTZViewControl::device() const
{
	OBSSourceAutoRelease source;
	{
		QMutexLocker locker(&m_sourceMutex);
		source = obs_weak_source_get_source(m_source);
	}
	if (!source || !ptzDeviceList)
		return QModelIndex();
	return ptzDeviceList->indexFromName(QString::fromUtf8(obs_source_get_name(source)));
}

/* The dock's "block moves of live cameras" applies here too */
bool PTZViewControl::liveLocked(const QModelIndex &index) const
{
	auto controls = PTZControls::getInstance();
	return controls && controls->liveMoveLockActive() && index.data(PTZListModel::IsLockedRole).toBool();
}

/* Where the camera is, if the device reads it back. Zoom is 0 if it doesn't
 * report that. */
bool PTZViewControl::readPosition(const QModelIndex &index, double &pan, double &tilt, double &zoom) const
{
	OBSDataAutoRelease state = obs_data_create();
	ptzDeviceList->saveState(index, state.Get());
	if (!obs_data_has_user_value(state, "pan") || !obs_data_has_user_value(state, "tilt"))
		return false;
	pan = obs_data_get_double(state, "pan");
	tilt = obs_data_get_double(state, "tilt");
	zoom = obs_data_get_double(state, "zoom");
	return true;
}

void PTZViewControl::trace(const QString &what)
{
	if (what == m_lastTrace)
		return;
	m_lastTrace = what;
	blog(LOG_INFO, "[ptz] view control: %s", QT_TO_UTF8(what));
}

static QString describe(const QModelIndex &index)
{
	OBSDataAutoRelease state = obs_data_create();
	ptzDeviceList->saveState(index, state.Get());
	auto has = [&](const char *key) {
		return obs_data_has_user_value(state, key) ? QString::number(obs_data_get_double(state, key))
							   : QString("none");
	};
	return QString("device '%1' connected=%2 live=%3 locked=%4 pantilt_abs=%5 zoom_abs=%6 pan=%7 tilt=%8 zoom=%9")
		.arg(index.data(Qt::DisplayRole).toString())
		.arg(obs_data_get_bool(state, "connected"))
		.arg(obs_data_get_bool(state, "live"))
		.arg(obs_data_get_bool(state, "locked"))
		.arg(PTZListModel::hasFeature(index, "pantilt_abs"))
		.arg(PTZListModel::hasFeature(index, "zoom_abs"))
		.arg(has("pan"), has("tilt"), has("zoom")) +
	       QString(" pan_pos=%1 zoom_pos=%2 statistics=%3")
		       .arg(has("pan_pos"), has("zoom_pos"))
		       .arg(QString::fromUtf8(obs_data_get_json(OBSDataAutoRelease(obs_data_get_obj(state, "statistics")))));
}

void PTZViewControl::setSource(obs_source_t *source)
{
	QMutexLocker locker(&m_sourceMutex);
	if (source ? obs_weak_source_references_source(m_source, source) : !m_source)
		return;
	m_source = OBSGetWeakRef(source);
	m_dragging = false;
	m_wheelDelta = 0;
	m_lastZoom.invalidate();
}

void PTZViewControl::resizeEvent(QResizeEvent *event)
{
	QWidget::resizeEvent(event);
	if (m_display) {
		QSize size = this->size() * devicePixelRatioF();
		obs_display_resize(m_display, size.width(), size.height());
	}
}

void PTZViewControl::showEvent(QShowEvent *event)
{
	QWidget::showEvent(event);
	createDisplay();
	if (m_display)
		obs_display_set_enabled(m_display, true);
}

/* Nothing to draw for while it can't be seen, as on another tab */
void PTZViewControl::hideEvent(QHideEvent *event)
{
	QWidget::hideEvent(event);
	if (m_display)
		obs_display_set_enabled(m_display, false);
	m_dragging = false;
}

void PTZViewControl::mousePressEvent(QMouseEvent *event)
{
	QWidget::mousePressEvent(event);
	if (event->button() != Qt::LeftButton)
		return;
	auto index = device();
	if (!index.isValid()) {
		trace("press: no device for this source");
		return;
	}
	trace("press: " + describe(index));
	if (liveLocked(index) || !PTZListModel::hasFeature(index, "pantilt_abs"))
		return;
	if (!readPosition(index, m_panStart, m_tiltStart, m_dragZoom))
		return;
	m_dragging = true;
	m_dragMoved = false;
	m_dragStart = m_dragPos = event->position();
}

void PTZViewControl::mouseMoveEvent(QMouseEvent *event)
{
	QWidget::mouseMoveEvent(event);
	if (!m_dragging)
		return;
	m_dragPos = event->position();
	m_dragMoved = true;
}

void PTZViewControl::mouseReleaseEvent(QMouseEvent *event)
{
	QWidget::mouseReleaseEvent(event);
	if (event->button() != Qt::LeftButton || !m_dragging)
		return;
	/* Send where it was let go, which the last tick may not have */
	m_dragPos = event->position();
	m_dragMoved = true;
	tick();
	m_dragging = false;
}

void PTZViewControl::wheelEvent(QWheelEvent *event)
{
	m_wheelDelta += event->angleDelta().y();
	trace(QString("wheel: %1").arg(event->angleDelta().y()));
	event->accept();
}

void PTZViewControl::tick()
{
	auto index = device();
	if ((m_wheelDelta || m_dragging) && !index.isValid())
		trace("tick: no device for this source");
	else if ((m_wheelDelta || m_dragging) && liveLocked(index))
		trace("tick: live locked");
	else if (m_wheelDelta && !PTZListModel::hasFeature(index, "zoom_abs"))
		trace("tick: no zoom_abs; " + describe(index));
	if (!index.isValid() || liveLocked(index)) {
		m_wheelDelta = 0;
		m_dragging = false;
		return;
	}

	if (m_wheelDelta && PTZListModel::hasFeature(index, "zoom_abs")) {
		if (!m_lastZoom.isValid() || m_lastZoom.elapsed() > zoomSettleMs) {
			OBSDataAutoRelease state = obs_data_create();
			ptzDeviceList->saveState(index, state.Get());
			if (!obs_data_has_user_value(state, "zoom")) {
				trace("tick: no zoom position; " + describe(index));
				m_wheelDelta = 0;
				return;
			}
			m_zoomTarget = obs_data_get_double(state, "zoom");
		}
		/* 120 is one notch of an ordinary wheel */
		m_zoomTarget = std::clamp(m_zoomTarget + m_wheelDelta / 120.0 * zoomPerNotch, 0.0, 1.0);
		m_wheelDelta = 0;
		m_lastZoom.start();

		trace(QString("tick: zoom to %1").arg(m_zoomTarget));
		calldata_t cd = {};
		calldata_set_float(&cd, "zoom", m_zoomTarget);
		ptzDeviceList->callDevice(index, "ptz_move_abs", &cd);
		calldata_free(&cd);
	}

	if (m_dragging && m_dragMoved) {
		m_dragMoved = false;
		QRectF rect = sourceRect();
		if (rect.isEmpty()) {
			trace("tick: source has no size");
			return;
		}
		/* The camera turns the way the mouse moves, by as much of its
		 * field of view as the mouse moved across the picture */
		double magnification = 1.0 + (m_config.maxMagnification - 1.0) * m_dragZoom;
		double dx = (m_dragPos.x() - m_dragStart.x()) / rect.width();
		double dy = (m_dragStart.y() - m_dragPos.y()) / rect.height();
		double pan = std::clamp(m_panStart + dx * m_config.panPerWidth / magnification, -1.0, 1.0);
		double tilt = std::clamp(m_tiltStart + dy * m_config.tiltPerHeight / magnification, -1.0, 1.0);

		trace(QString("tick: pan/tilt to %1, %2").arg(pan).arg(tilt));
		calldata_t cd = {};
		calldata_set_float(&cd, "pan", pan);
		calldata_set_float(&cd, "tilt", tilt);
		ptzDeviceList->callDevice(index, "ptz_move_abs", &cd);
		calldata_free(&cd);
	}
}
