/* Touch control widget
 *
 * Copyright 2020 Grant Likely <grant.likely@secretlab.ca>
 *
 * SPDX-License-Identifier: GPLv2
 */

#include <QWidget>
#include <QPainter>
#include <QMouseEvent>
#include <util/base.h>
#include <obs-module.h>
#include <cmath>
#include "touch-control.hpp"

TouchControl::TouchControl(QWidget *parent, Qt::WindowFlags f)
	: QWidget(parent, f),
	  m_x(0),
	  m_y(0),
	  m_inner_radius(0.3),
	  m_outer_radius(0.99)
{
}

double TouchControl::x() const
{
	return m_x;
}

double TouchControl::y() const
{
	return m_y;
}

void TouchControl::setPosition(QPointF pos)
{
	double size = (std::min)(width(), height()) / 2;
	double x = std::clamp((pos.x() - width() / 2) / size, -1.0, 1.0);
	double y = -std::clamp((pos.y() - height() / 2) / size, -1.0, 1.0);
	if (x != m_x || y != m_y) {
		m_x = x;
		m_y = y;
		update();
		emit positionChanged(m_x, m_y);
	}
}

void TouchControl::paintEvent(QPaintEvent *event)
{
	Q_UNUSED(event);
	QPainter painter(this);

	painter.setRenderHint(QPainter::Antialiasing);
	painter.setPen(QPen(QBrush(Qt::gray), 0.01));
	auto tsize = (std::min)(width(), height()) / 2;
	painter.translate(QPointF(width() / 2, height() / 2));
	painter.scale(tsize, tsize);

	// Draw an outer guide circle
	auto p = QPointF(1, 1) * m_outer_radius;
	painter.drawArc(QRectF(-p, p), 0, 360 * 16);

	// Draw the joystick current position
	double mag = std::clamp(sqrt(m_x * m_x + m_y * m_y), 0.0, 1.0 - m_inner_radius);
	double angle = atan2(-m_y, m_x) * 180 / M_PI;
	p = QPointF(1, 1) * m_inner_radius;
	painter.rotate(angle);
	painter.translate(QPointF(mag, 0));
	painter.drawArc(QRectF(-p, p), 0, 360 * 16);
}

void TouchControl::mousePressEvent(QMouseEvent *event)
{
	if (event->button() == Qt::LeftButton)
		setPosition(event->pos());
}

void TouchControl::mouseReleaseEvent(QMouseEvent *event)
{
	if (event->button() == Qt::LeftButton) {
		m_x = 0;
		m_y = 0;
		update();
		emit positionChanged(0, 0);
	}
}

void TouchControl::mouseMoveEvent(QMouseEvent *event)
{
	if (event->buttons() & Qt::LeftButton)
		setPosition(event->pos());
}
