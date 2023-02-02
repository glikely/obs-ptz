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
	  m_inner_radius(0.2),
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
	double size = std::min(width(), height()) / 2;
	double x = std::clamp((pos.x() - width() / 2) / size, -1.0, 1.0);
	double y = -std::clamp((pos.y() - height() / 2) / size, -1.0, 1.0);
	double mag = sqrt(x * x + y * y);
	if (mag < m_inner_radius) {
		x = 0;
		y = 0;
	} else {
		// Scale to active range
		mag = (mag - m_inner_radius) / (1 - m_inner_radius);
		double angle = atan2(y, x);
		// Constrain to 45 or 22.5 degree sectors
		const double sector = M_PI / (mag < 0.5 ? 4 : 8);
		angle = round(angle / sector) * sector;
		x = mag * cos(angle);
		y = mag * sin(angle);
	}
	if (x != m_x || y != m_y) {
		m_x = x;
		m_y = y;
		emit positionChanged(m_x, m_y);
	}
}

void TouchControl::paintEvent(QPaintEvent *event)
{
	Q_UNUSED(event);
	QPainter painter(this);

	painter.setRenderHint(QPainter::Antialiasing);
	painter.setPen(QPen(QBrush(Qt::gray), 0.01));
	auto tsize = std::min(width(), height()) / 2;
	painter.translate(QPointF(width() / 2, height() / 2));
	painter.scale(tsize, tsize);

	// Draw three guide circles; inner, middle, and outer
	auto p = QPointF(1, 1) * m_inner_radius;
	painter.drawArc(QRectF(-p, p), 0, 360 * 16);
	p = QPointF(1, 1) * (m_outer_radius + m_inner_radius) / 2;
	painter.drawArc(QRectF(-p, p), 0, 360 * 16);
	p = QPointF(1, 1) * m_outer_radius;
	painter.drawArc(QRectF(-p, p), 0, 360 * 16);

	// Draw directional arrows between the circles
	painter.setPen(QPen(QBrush(Qt::gray), 0.05));
	QPolygonF arrow;
	arrow << QPointF(-0.15, -0.05) << QPointF(0, 0.05)
	      << QPointF(0.15, -0.05);
	for (int i = 0; i < 4; i++) {
		painter.save();
		painter.rotate(90 * i);
		double delta = (m_outer_radius - m_inner_radius) / 4;
		painter.translate(QPointF(0, m_inner_radius + delta));
		painter.drawPolyline(arrow);
		painter.translate(QPointF(0, delta * 2 - 0.05));
		painter.drawPolyline(arrow);
		painter.translate(QPointF(0, 0.10));
		painter.drawPolyline(arrow);
		painter.restore();
	}
}

void TouchControl::mousePressEvent(QMouseEvent *event)
{
	setPosition(event->position());
}

void TouchControl::mouseReleaseEvent(QMouseEvent *)
{
	m_x = 0;
	m_y = 0;
	emit positionChanged(m_x, m_y);
}

void TouchControl::mouseMoveEvent(QMouseEvent *event)
{
	setPosition(event->position());
}
