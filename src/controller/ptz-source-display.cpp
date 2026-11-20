/* Pan Tilt Zoom Controls - a source's video at the full frame rate
 *
 * Copyright 2026 Grant Likely <grant.likely@secretlab.ca>
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include <obs-module.h>
#include <graphics/graphics.h>
#include <graphics/matrix4.h>
#include <QItemSelectionRange>
#include <QPainter>
#include <QTimer>
#include <QWindow>

#include "ptz-source-display.hpp"
#include "display-helpers.hpp"
#include "ptz-list-model.hpp"

PTZSourceDisplay::PTZSourceDisplay(QWidget *parent) : OBSQTDisplay(parent)
{
	setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);

	/* The display is made once the window is on screen, and again if it
	 * moves to another screen, so each time it is made is when to say what
	 * draws into it */
	connect(this, &OBSQTDisplay::DisplayCreated, this,
		[this]() { obs_display_add_draw_callback(GetDisplay(), draw, this); });

	/* A device can be moved to another source, which changes what it shows */
	connect(ptzDeviceList, &PTZListModel::dataChanged, this,
		[this](const QModelIndex &topLeft, const QModelIndex &bottomRight) {
			if (QItemSelectionRange(topLeft, bottomRight).contains(QModelIndex(m_device))) {
				setSource(ptzDeviceList->parentSource(m_device));
				updateOverlay();
			}
		});
}

PTZSourceDisplay::~PTZSourceDisplay()
{
	unwatchAncestors();
	/* The display waits for a draw in progress when it goes, and must go
	 * before what the draw callback reads, which is members of this class,
	 * does: OBSQTDisplay would only do it after they are gone. */
	DestroyDisplay();
	releaseShowing();
	if (m_overlayTexture) {
		obs_enter_graphics();
		gs_texture_destroy(m_overlayTexture);
		obs_leave_graphics();
	}
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
	std::lock_guard<std::mutex> guard(m_lock);
	holdShowingLocked();
}

void PTZSourceDisplay::hideEvent(QHideEvent *event)
{
	OBSQTDisplay::hideEvent(event);
	unwatchAncestors();
	releaseShowing();
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

/* Keeping a source showing isn't free: a network camera's source only connects
 * while it is, so one that nothing can see is left alone */
void PTZSourceDisplay::holdShowingLocked()
{
	if (m_showing || !isVisible())
		return;
	OBSSourceAutoRelease source = obs_weak_source_get_source(m_source);
	if (!source)
		return;
	obs_source_inc_showing(source);
	m_showing = true;
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

void PTZSourceDisplay::setDevice(const QModelIndex &index)
{
	m_device = index;
	setSource(ptzDeviceList->parentSource(index));
	updateOverlay();
}

/* Only paints when the text changes, as a moving camera reports all the time */
void PTZSourceDisplay::updateOverlay()
{
	QString text;
	if (m_device.isValid()) {
		const char *status = "PTZ.Device.Status.Connected";
		if (!m_device.data(PTZListModel::IsConnectedRole).toBool())
			status = "PTZ.Device.Status.Disconnected";
		else if (m_device.data(PTZListModel::IsPoweredOffRole).toBool())
			status = "PTZ.Device.Status.PoweredOff";
		text = m_device.data(Qt::DisplayRole).toString() + "\n" + obs_module_text(status);
	}
	if (text == m_overlayText)
		return;
	m_overlayText = text;

	QImage image = text.isEmpty() ? QImage() : paintOverlay(text);
	std::lock_guard<std::mutex> guard(m_lock);
	m_overlayImage = image;
	m_overlayChanged = true;
}

/* In the byte order of GS_BGRA, with its alpha premultiplied, for draw() */
QImage PTZSourceDisplay::paintOverlay(const QString &text) const
{
	const int padding = 6;
	const QSize size = fontMetrics().size(0, text) + QSize(padding * 2, padding * 2);
	const qreal ratio = devicePixelRatioF();

	QImage image(size * ratio, QImage::Format_ARGB32_Premultiplied);
	image.setDevicePixelRatio(ratio);
	image.fill(Qt::transparent);

	QPainter painter(&image);
	painter.setRenderHint(QPainter::Antialiasing);
	painter.setPen(Qt::NoPen);
	painter.setBrush(QColor(0, 0, 0, 160));
	painter.drawRoundedRect(QRect(QPoint(0, 0), size), 4, 4);
	painter.setPen(Qt::white);
	painter.setFont(font());
	painter.drawText(QRect(QPoint(0, 0), size).adjusted(padding, padding, -padding, -padding), text);
	return image;
}

void PTZSourceDisplay::setSource(OBSSource source)
{
	OBSWeakSource weak = OBSGetWeakRef(source);
	std::lock_guard<std::mutex> guard(m_lock);
	if (m_source.Get() == weak.Get())
		return;
	releaseShowingLocked();
	m_source = weak;
	holdShowingLocked();
}

QString PTZSourceDisplay::shownSource() const
{
	std::lock_guard<std::mutex> guard(m_lock);
	OBSSource source = OBSGetStrongRef(m_source);
	return QString::fromUtf8(obs_source_get_name(source));
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

static void drawVideo(obs_source_t *source, uint32_t cx, uint32_t cy)
{
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

/* In the display's own pixels, so it is the same size whatever the video's */
static void drawOverlay(gs_texture_t *texture)
{
	const float margin = 8.0f;
	gs_effect_t *effect = obs_get_base_effect(OBS_EFFECT_PREMULTIPLIED_ALPHA);

	gs_blend_state_push();
	gs_blend_function(GS_BLEND_ONE, GS_BLEND_INVSRCALPHA);
	gs_matrix_push();
	gs_matrix_translate3f(margin, margin, 0.0f);
	gs_effect_set_texture(gs_effect_get_param_by_name(effect, "image"), texture);
	while (gs_effect_loop(effect, "Draw"))
		gs_draw_sprite(texture, 0, gs_texture_get_width(texture), gs_texture_get_height(texture));
	gs_matrix_pop();
	gs_blend_state_pop();
}

/* Runs on the graphics thread, once a frame, with the display's own size */
void PTZSourceDisplay::draw(void *param, uint32_t cx, uint32_t cy)
{
	auto *self = static_cast<PTZSourceDisplay *>(param);
	OBSSourceAutoRelease source;
	QImage overlay;
	bool overlayChanged;
	{
		std::lock_guard<std::mutex> guard(self->m_lock);
		source = obs_weak_source_get_source(self->m_source);
		overlayChanged = self->m_overlayChanged;
		self->m_overlayChanged = false;
		overlay = self->m_overlayImage;
	}
	if (overlayChanged) {
		if (self->m_overlayTexture)
			gs_texture_destroy(self->m_overlayTexture);
		const uint8_t *bits = overlay.constBits();
		self->m_overlayTexture =
			overlay.isNull() ? nullptr
					 : gs_texture_create(overlay.width(), overlay.height(), GS_BGRA, 1, &bits, 0);
	}
	if (!cx || !cy)
		return;

	if (source)
		drawVideo(source, cx, cy);
	if (self->m_overlayTexture)
		drawOverlay(self->m_overlayTexture);
}
