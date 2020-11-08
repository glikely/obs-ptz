/* Pan Tilt Zoom device base object
 *
 * Copyright 2020 Grant Likely <grant.likely@secretlab.ca>
 *
 * SPDX-License-Identifier: GPLv2
 */
#pragma once

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
