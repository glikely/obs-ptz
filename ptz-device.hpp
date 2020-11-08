/* Pan Tilt Zoom device base object
 *
 * Copyright 2020 Grant Likely <grant.likely@secretlab.ca>
 *
 * SPDX-License-Identifier: GPLv2
 */
#pragma once

#include <QDebug>
#include <QObject>
#include <QtGlobal>

class PTZDevice : public QObject {
	Q_OBJECT

public:
	PTZDevice() : QObject() { };
	~PTZDevice() { };

	virtual void pantilt(double pan, double tilt) { Q_UNUSED(pan); Q_UNUSED(tilt); }
	virtual void pantilt_stop() { }
	virtual void pantilt_up(uint32_t pan_speed, uint32_t tilt_speed) { Q_UNUSED(pan_speed); Q_UNUSED(tilt_speed); }
	virtual void pantilt_upleft(uint32_t pan_speed, uint32_t tilt_speed) { Q_UNUSED(pan_speed); Q_UNUSED(tilt_speed); }
	virtual void pantilt_upright(uint32_t pan_speed, uint32_t tilt_speed) { Q_UNUSED(pan_speed); Q_UNUSED(tilt_speed); }
	virtual void pantilt_left(uint32_t pan_speed, uint32_t tilt_speed) { Q_UNUSED(pan_speed); Q_UNUSED(tilt_speed); }
	virtual void pantilt_right(uint32_t pan_speed, uint32_t tilt_speed) { Q_UNUSED(pan_speed); Q_UNUSED(tilt_speed); }
	virtual void pantilt_down(uint32_t pan_speed, uint32_t tilt_speed) { Q_UNUSED(pan_speed); Q_UNUSED(tilt_speed); }
	virtual void pantilt_downleft(uint32_t pan_speed, uint32_t tilt_speed) { Q_UNUSED(pan_speed); Q_UNUSED(tilt_speed); }
	virtual void pantilt_downright(uint32_t pan_speed, uint32_t tilt_speed) { Q_UNUSED(pan_speed); Q_UNUSED(tilt_speed); }
	virtual void pantilt_home() { }
	virtual void zoom_stop() { }
	virtual void zoom_tele() { }
	virtual void zoom_wide() { }
};

class PTZSimulator : public PTZDevice {
	Q_OBJECT

public:
	void pantilt(double pan, double tilt) { qDebug() << __func__ << "Pan" << pan << "Tilt" << tilt; }
	void pantilt_stop() { qDebug() << __func__; }
	void pantilt_up(uint32_t pan_speed, uint32_t tilt_speed) { qDebug() << __func__ << "Pan" << pan_speed << "Tilt" << tilt_speed; }
	void pantilt_upleft(uint32_t pan_speed, uint32_t tilt_speed) { qDebug() << __func__ << "Pan" << pan_speed << "Tilt" << tilt_speed; }
	void pantilt_upright(uint32_t pan_speed, uint32_t tilt_speed) { qDebug() << __func__ << "Pan" << pan_speed << "Tilt" << tilt_speed; }
	void pantilt_left(uint32_t pan_speed, uint32_t tilt_speed) { qDebug() << __func__ << "Pan" << pan_speed << "Tilt" << tilt_speed; }
	void pantilt_right(uint32_t pan_speed, uint32_t tilt_speed) { qDebug() << __func__ << "Pan" << pan_speed << "Tilt" << tilt_speed; }
	void pantilt_down(uint32_t pan_speed, uint32_t tilt_speed) { qDebug() << __func__ << "Pan" << pan_speed << "Tilt" << tilt_speed; }
	void pantilt_downleft(uint32_t pan_speed, uint32_t tilt_speed) { qDebug() << __func__ << "Pan" << pan_speed << "Tilt" << tilt_speed; }
	void pantilt_downright(uint32_t pan_speed, uint32_t tilt_speed) { qDebug() << __func__ << "Pan" << pan_speed << "Tilt" << tilt_speed; }
	void pantilt_home() { qDebug() << __func__; }
	void zoom_stop() { qDebug() << __func__; }
	void zoom_tele() { qDebug() << __func__; }
	void zoom_wide() { qDebug() << __func__; }
};
